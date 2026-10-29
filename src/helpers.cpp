// Helpers behind helpers.h: helper.ini, command lines, child processes, trust records, the decoder
// that reads a helper's WAV output, tag reading and the visual feed. Nothing here touches the window
// or the library; the app hands in copies and gets plain values back.
#include "helpers.h"

#include "audio_file.h"
#include "command_line.h"
#include "library.h"
#include "packs.h"
#include "sort.h"

#ifdef _WIN32
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <mutex>

namespace {

constexpr size_t kMaxManifest = 64 << 10, kMaxTrustFiles = 256;
constexpr size_t kMaxTools = 16, kMaxVisualizers = 4;
#ifdef _WIN32
const wchar_t kSlash[] = L"\\";
const wchar_t kSlashStar[] = L"\\*";
#else
const wchar_t kSlash[] = L"/";
const wchar_t kSlashStar[] = L"/*";
#endif

// File types that are never a helper's besides cordinal's own audio: lists, sheets and packs.
const wchar_t* const kReserved[] = {L"cue", L"m3u", L"m3u8", L"zip", L"cordpack", L"lrc"};

std::atomic<unsigned> g_patience{15000};

std::mutex g_activeMx;
std::shared_ptr<const std::vector<std::shared_ptr<const Helper>>> g_active;

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n"), e = s.find_last_not_of(L" \t\r\n");
    return b == std::wstring::npos ? L"" : s.substr(b, e - b + 1);
}

bool EndsWith(const std::wstring& s, const wchar_t* tail) {
    size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

// "a", "a and b", "a, b and c"
std::wstring JoinWords(const std::vector<std::wstring>& words) {
    std::wstring out;
    for (size_t i = 0; i < words.size(); ++i) out += (i == 0 ? L"" : i + 1 == words.size() ? L" and " : L", ") + words[i];
    return out;
}

// What a program printed: UTF-8 when it is valid UTF-8, else the console's code page, which is what
// older Windows tools write.
#ifdef _WIN32
std::wstring ProgramText(const std::string& bytes) {
    if (bytes.empty()) return L"";
    UINT page = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0) ? CP_UTF8 : GetOEMCP();
    int n = MultiByteToWideChar(page, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(page, 0, bytes.data(), (int)bytes.size(), w.data(), n);
    return w;
}

std::wstring SystemText(DWORD code) {
    wchar_t text[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text, (DWORD)std::size(text), nullptr);
    std::wstring s = Trim(text);
    if (!s.empty() && s.back() == L'.') s.pop_back();
    return s.empty() ? L"error " + std::to_wstring(code) : s;
}

#else
// What a program printed, as text: UTF-8 when it is valid, else Latin-1, which is what older tools write.
std::wstring ProgramText(const std::string& bytes) {
    if (bytes.empty()) return L"";
    std::wstring w = FromUtf8(bytes);
    if (ToUtf8(w) == bytes) return w;
    w.clear();
    for (unsigned char c : bytes) w += (wchar_t)c;
    return w;
}

std::wstring SystemText(DWORD code) {
    std::string text = strerror((int)code);
    std::wstring s = Trim(FromUtf8(text));
    return s.empty() ? L"error " + std::to_wstring(code) : s;
}

#endif

bool ReadSmallFile(const std::wstring& path, size_t limit, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && (uint64_t)size.QuadPart <= limit;
    DWORD got = 0;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        ok = out.empty() || (ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    return ok;
}

#ifdef _WIN32
// Every helper program joins this job. Closing cordinal closes the job's last handle, which ends
// them all, and a program that crashes ends quietly instead of showing Windows' crash dialog.
HANDLE Job() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (j) SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof(info));
        return j;
    }();
    return job;
}

#endif

// Splits a command line into words: spaces separate them, and double quotes group a word with
// spaces in it (the quotes themselves are dropped).
std::vector<std::wstring> SplitWords(const std::wstring& line) {
    std::vector<std::wstring> words;
    std::wstring word;
    bool quoted = false, any = false;
    for (wchar_t c : line) {
        if (c == L'"') {
            quoted = !quoted;
            any = true;
        } else if ((c == L' ' || c == L'\t') && !quoted) {
            if (any) words.push_back(word);
            word.clear();
            any = false;
        } else {
            word += c;
            any = true;
        }
    }
    if (any) words.push_back(word);
    return words;
}

// The placeholders in one argument, without their braces; "{{" and "}}" are literal braces.
std::vector<std::wstring> Placeholders(const std::wstring& arg) {
    std::vector<std::wstring> names;
    for (size_t i = 0; i < arg.size(); ++i) {
        if ((arg[i] == L'{' || arg[i] == L'}') && i + 1 < arg.size() && arg[i + 1] == arg[i]) ++i;
        else if (arg[i] == L'{') {
            size_t close = arg.find(L'}', i);
            if (close == std::wstring::npos) break;
            names.push_back(arg.substr(i + 1, close - i - 1));
            i = close;
        }
    }
    return names;
}

bool IsTitleFormat(const std::wstring& placeholder) { return placeholder.find_first_of(L"%$") != std::wstring::npos; }

// The placeholders a kind of command may use: only a tool has tracks to format, only a decoder a
// place to start, only a visualizer the feed.
enum Uses : unsigned { kFile = 1, kFiles = 2, kStart = 4, kFeed = 8, kFormat = 16 };

// A command from helper.ini. The program must be an .exe in the helper's folder: never a path
// outside it, and never a batch file, which cmd.exe would reread with other quoting rules.
bool ParseCommand(const std::wstring& line, const std::wstring& dir, unsigned uses, HelperCommand& out, std::wstring& error) {
    auto words = SplitWords(line);
    if (words.empty()) return error = L"it is empty", false;
    std::wstring program = words[0];
#ifdef _WIN32
    std::replace(program.begin(), program.end(), L'/', L'\\');
    bool inside = !program.empty() && program[0] != L'\\' && program.find(L':') == std::wstring::npos &&
                  program.find(L"..") == std::wstring::npos;
    if (!inside || !EndsWith(program, L".exe")) return error = program + L" isn't a program (.exe) in the helper's folder", false;
    DWORD a = GetFileAttributesW((dir + kSlash + program).c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return error = L"its folder has no " + program, false;
#else
    // One helper.ini serves both systems: a program written "name.exe" is the file "name" here.
    if (EndsWith(program, L".exe")) program.resize(program.size() - 4);
    bool inside = !program.empty() && program[0] != L'/' && program.find(L"..") == std::wstring::npos;
    if (!inside) return error = program + L" isn't a program in the helper's folder", false;
    struct stat st {};
    if (stat(ToUtf8(dir + L"/" + program).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return error = L"its folder has no " + program, false;
    if (!(st.st_mode & 0111)) return error = program + L" isn't executable (chmod +x)", false;
#endif
    for (size_t i = 1; i < words.size(); ++i)
        for (const auto& name : Placeholders(words[i])) {
            const bool alone = words[i] == L"{files}";
            const unsigned use = name == L"file" ? kFile : name == L"files" && alone ? kFiles : name == L"start" ? kStart
                               : name == L"feed" ? kFeed : IsTitleFormat(name) ? kFormat : 0;
            if (use & uses) continue;
            error = L"it uses {" + name + L"}, which cordinal doesn't fill in" +
                    (name == L"files" && !alone ? L" inside a longer argument" : use ? L" there" : L"");
            return false;
        }
    out.program = program;  // only a command that passed: a rejected one must not run
    out.args.assign(words.begin() + 1, words.end());
    return true;
}

std::wstring FormatSeconds(double seconds) {
    wchar_t text[48];
    swprintf_s(text, L"%.6f", std::max(0.0, seconds));
    return text;
}

// A value as one argument: quoted only when it has to be.
std::wstring Argument(const std::wstring& value) {
    return value.empty() || value.find_first_of(L" \t\"") != std::wstring::npos ? QuoteArg(value) : value;
}

// --- Trust ---

struct TrustFile {
    std::wstring name;
    uint64_t size = 0, time = 0;
    std::wstring hash;
};

// Whether a file is one of the programs or libraries a helper runs: .exe and .dll on Windows; on Linux
// every executable file and every shared library.
bool TrustedProgram(const std::wstring& path, const std::wstring& name) {
#ifdef _WIN32
    (void)path;
    return EndsWith(name, L".exe") || EndsWith(name, L".dll");
#else
    struct stat st {};
    if (stat(ToUtf8(path).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    return (st.st_mode & 0111) || EndsWith(name, L".so") || name.find(L".so.") != std::wstring::npos;
#endif
}

// helper.ini, and every program and library in the folder and two folders under it: everything that
// decides what runs.
void ListTrusted(const std::wstring& dir, const std::wstring& prefix, int depth, std::vector<TrustFile>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + kSlashStar).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 2) ListTrusted(dir + kSlash + name, prefix + name + kSlash, depth + 1, out);
        } else if (out.size() < kMaxTrustFiles &&
                   ((prefix.empty() && _wcsicmp(name.c_str(), L"helper.ini") == 0) || TrustedProgram(dir + kSlash + name, name))) {
            out.push_back({prefix + name, (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow,
                           (uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime, L""});
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<TrustFile> TrustedFiles(const std::wstring& dir) {
    std::vector<TrustFile> files;
    ListTrusted(dir, L"", 0, files);
    SortObjects(files.begin(), files.end(), [](auto& a, auto& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return files;
}

#ifdef _WIN32
std::wstring Sha256(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0));
    std::vector<uint8_t> buffer(1 << 20);
    DWORD got = 0;
    while (ok && (ok = ReadFile(f, buffer.data(), (DWORD)buffer.size(), &got, nullptr) != 0) && got)
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), got, 0));
    uint8_t digest[32]{};
    if (hash) {
        ok = BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0)) && ok;
        BCryptDestroyHash(hash);
    }
    CloseHandle(f);
    if (!ok) return L"";
    std::wstring hex;
    for (uint8_t b : digest) {
        hex += L"0123456789abcdef"[b >> 4];
        hex += L"0123456789abcdef"[b & 15];
    }
    return hex;
}

#else
// SHA-256 of a file, as lowercase hex; empty when it can't be read.
std::wstring Sha256(const std::wstring& path) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    FILE* f = fopen(ToUtf8(path).c_str(), "rb");
    if (!f) return L"";
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], ff = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & ff) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = ff, ff = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += ff, h[6] += g, h[7] += hh;
    };
    std::vector<uint8_t> buffer(1 << 20);
    uint64_t total = 0;
    size_t have = 0;
    for (size_t got; (got = fread(buffer.data() + have, 1, buffer.size() - have, f)) > 0;) {
        total += got;
        have += got;
        size_t whole = have / 64 * 64;
        for (size_t i = 0; i < whole; i += 64) block(buffer.data() + i);
        std::memmove(buffer.data(), buffer.data() + whole, have - whole);
        have -= whole;
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) return L"";
    buffer[have++] = 0x80;
    if (have > 56) {
        std::memset(buffer.data() + have, 0, 64 - have);
        block(buffer.data());
        have = 0;
    }
    std::memset(buffer.data() + have, 0, 56 - have);
    for (int i = 0; i < 8; ++i) buffer[56 + i] = (uint8_t)((total * 8) >> (56 - 8 * i));
    block(buffer.data());
    std::wstring hex;
    for (uint32_t v : h)
        for (int shift = 28; shift >= 0; shift -= 4) hex += L"0123456789abcdef"[(v >> shift) & 15];
    return hex;
}
#endif

// name*size*time*sha256, '|' between files: neither '*' nor '|' can be in a file name.
std::wstring TrustRecord(const std::vector<TrustFile>& files) {
    std::wstring out;
    for (const auto& f : files)
        out += (out.empty() ? L"" : L"|") + f.name + L"*" + std::to_wstring(f.size) + L"*" + std::to_wstring(f.time) + L"*" + f.hash;
    return out;
}

std::vector<TrustFile> ParseTrust(const std::wstring& record) {
    std::vector<TrustFile> files;
    for (size_t at = 0; at < record.size();) {
        size_t bar = std::min(record.find(L'|', at), record.size());
        std::wstring item = record.substr(at, bar - at);
        size_t a = item.find(L'*'), b = item.find(L'*', a + 1), c = item.find(L'*', b + 1);
        if (a == std::wstring::npos || b == std::wstring::npos || c == std::wstring::npos) return {};
        files.push_back({item.substr(0, a), wcstoull(item.c_str() + a + 1, nullptr, 10), wcstoull(item.c_str() + b + 1, nullptr, 10),
                         item.substr(c + 1)});
        at = bar + 1;
    }
    return files;
}

std::shared_ptr<const std::vector<std::shared_ptr<const Helper>>> ActiveHelpers() {
    std::lock_guard<std::mutex> lock(g_activeMx);
    return g_active;
}

std::wstring HelperName(const Helper& h) { return L"the " + h.name + L" helper"; }

// --- The decoder ---

uint32_t Le16(const char* p) { return (uint8_t)p[0] | (uint8_t)p[1] << 8; }
uint32_t Le32(const char* p) { return Le16(p) | Le16(p + 2) << 16; }

// Plays a file through the helper's decode command, which writes it to stdout as a WAV stream:
// integer PCM of 8 to 32 bits or float of 32 or 64, one to eight channels. Seeking starts the
// program again at the new place ({start}); for a command without {start} it starts from the top
// and cordinal skips ahead.
class HelperDecoder final : public Decoder {
public:
    HelperDecoder(std::shared_ptr<const Helper> helper, std::wstring path) : helper_(std::move(helper)), path_(std::move(path)) {}

    bool Start(double seconds, std::wstring& error) {
        const bool seekable = helper_->decode.Has(L"start");
        HelperValues v;
        v.file = path_;
        v.start = seekable ? seconds : 0;
        auto next = std::make_unique<HelperProcess>();
        std::wstring why;
        if (aborted_ || !next->Start(helper_->dir + kSlash + helper_->decode.program, HelperArguments(helper_->decode, v), helper_->dir, why)) {
            error = HelperName(*helper_) + L" couldn't start" + (why.empty() ? L"" : L": " + why);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mx_);
            process_ = std::move(next);
        }
        carry_.clear();
        failed_ = ended_ = false;
        problem_.clear();
        const unsigned before = rate_;
        if (!ReadHeader(v.start, error)) return false;
        if (before && rate_ != before) return error = HelperName(*helper_) + L" changed its sample rate", false;
        skip_ = seekable ? 0 : (uint64_t)std::llround(std::max(0.0, seconds) * rate_);
        return true;
    }

    size_t Read(float* out, size_t frames) override {
        if (!frames || ended_ || failed_) return 0;
        const size_t frameBytes = (size_t)channels_ * bytes_;
        for (;;) {
            // Frames still to skip, on the way to a place the program couldn't seek to, are read in
            // large pieces; what is read beyond `frames` waits in carry_ for the next call.
            const size_t want = (size_t)std::min<uint64_t>(skip_ + std::min<size_t>(frames, 8192), 8192) * frameBytes;
            size_t have = carry_.size();
            if (buffer_.size() < std::max(want, have)) buffer_.resize(std::max(want, have));
            std::memcpy(buffer_.data(), carry_.data(), have);
            carry_.clear();
            while (have < frameBytes) {
                size_t n = process_->Read(buffer_.data() + have, want - have, GetTickCount64() + HelperPatience());
                if (!n) {
                    carry_.assign(buffer_.data(), have);
                    return End();
                }
                have += n;
            }
            const size_t whole = have / frameBytes, skipped = (size_t)std::min<uint64_t>(skip_, whole);
            const size_t given = std::min(whole - skipped, frames);
            skip_ -= skipped;
            carry_.assign(buffer_.data() + (skipped + given) * frameBytes, have - (skipped + given) * frameBytes);
            if (!given) continue;
            Convert(buffer_.data() + skipped * frameBytes, given, out);
            return given;
        }
    }

    bool Seek(double seconds) override {
        if (!std::isfinite(seconds)) return false;
        std::wstring error;
        if (!Start(std::max(0.0, seconds), error)) {
            failed_ = true;
            problem_ = error;
            return false;
        }
        return true;
    }
    double Duration() const override { return duration_; }
    unsigned Rate() const override { return rate_; }
    void Abort() override {
        aborted_ = true;
        std::lock_guard<std::mutex> lock(mx_);
        if (process_) process_->Stop();
    }
    bool Failed() const override { return failed_; }
    std::wstring Problem() const override { return problem_; }

private:
    bool ReadFully(char* to, size_t bytes) {
        for (size_t got = 0; got < bytes;) {
            size_t n = process_->Read(to + got, bytes - got, GetTickCount64() + HelperPatience());
            if (!n) return false;
            got += n;
        }
        return true;
    }

    bool ReadHeader(double start, std::wstring& error) {
        // What the program printed on its way out explains more than the bytes it wrote.
        auto fail = [&](const std::wstring& why) {
            const bool silent = process_->TimedOut();
            std::wstring said;
            if (!silent) {
                process_->Finish(1000);
                said = process_->Complaint();
            }
            error = HelperName(*helper_) + (silent ? L" didn't answer" : said.empty() ? why : L" stopped: " + said);
            return false;
        };
        char riff[12];
        if (!ReadFully(riff, 12) || std::memcmp(riff, "RIFF", 4) || std::memcmp(riff + 8, "WAVE", 4))
            return fail(L"'s output isn't a WAV stream");
        const uint32_t riffSize = Le32(riff + 4);
        uint64_t offset = 12;
        bool format = false;
        for (int chunk = 0; chunk < 64; ++chunk) {
            char head[8];
            if (!ReadFully(head, 8)) return fail(L"'s output ended in its WAV header");
            const uint32_t size = Le32(head + 4);
            offset += 8;
            if (!std::memcmp(head, "data", 4)) {
                if (!format) return fail(L"'s output has no WAV format before its samples");
                // A size the RIFF header agrees with gives the length. A program writing to a pipe
                // can't know it and leaves 0 or 0xFFFFFFFF; it is read to its end either way.
                if (size && size != 0xFFFFFFFF && riffSize != 0xFFFFFFFF && riffSize >= offset + size - 8 && !duration_) {
                    const uint64_t frames = size / ((uint64_t)channels_ * bytes_);
                    duration_ = start + (double)frames / rate_;
                }
                return true;
            }
            if (size > (1u << 20)) return fail(L"'s WAV header is too large");
            std::string body(size + (size & 1), '\0');  // chunks are padded to an even length
            if (!ReadFully(body.data(), body.size())) return fail(L"'s output ended in its WAV header");
            offset += body.size();
            if (std::memcmp(head, "fmt ", 4)) continue;
            if (size < 16) return fail(L"'s WAV format is damaged");
            uint32_t tag = Le16(body.data()), channels = Le16(body.data() + 2), rate = Le32(body.data() + 4), bits = Le16(body.data() + 14);
            if (tag == 0xFFFE && size >= 26) tag = Le16(body.data() + 24);  // WAVE_FORMAT_EXTENSIBLE's sub-format
            const bool pcm = tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32), real = tag == 3 && (bits == 32 || bits == 64);
            if (!pcm && !real) return fail(L" writes a sample format cordinal doesn't read");
            if (channels < 1 || channels > 8 || rate < 1000 || rate > 768000)
                return fail(L" writes a channel count or sample rate cordinal doesn't read");
            channels_ = channels;
            rate_ = rate;
            bytes_ = bits / 8;
            float_ = real;
            format = true;
        }
        return fail(L"'s WAV header is too long");
    }

    // The end of the program's output: the end of the track, or how the program failed.
    size_t End() {
        ended_ = true;
        if (aborted_) {
            failed_ = true;
            return 0;
        }
        if (process_->TimedOut()) {
            failed_ = true;
            problem_ = HelperName(*helper_) + L" stopped answering";
            return 0;
        }
        // A program that closed its output but lingers has still delivered all of it.
        DWORD code = process_->Finish(3000);
        if (code != 0 && code != ERROR_TIMEOUT) {
            std::wstring said = process_->Complaint();
            failed_ = true;
            problem_ = HelperName(*helper_) + L" stopped" + (said.empty() ? L"" : L": " + said);
        } else if (!carry_.empty()) {
            failed_ = true;
            problem_ = HelperName(*helper_) + L"'s output ended in the middle of a sample";
        }
        return 0;
    }

    void Convert(const char* in, size_t frames, float* out) const {
        float channels[8];
        for (size_t i = 0; i < frames; ++i) {
            for (unsigned c = 0; c < channels_; ++c, in += bytes_) {
                float v;
                if (float_ && bytes_ == 8) {
                    double d;
                    std::memcpy(&d, in, 8);
                    v = (float)d;
                } else if (float_) {
                    std::memcpy(&v, in, 4);
                } else if (bytes_ == 1) {
                    v = ((uint8_t)in[0] - 128) / 128.0f;
                } else if (bytes_ == 2) {
                    v = (int16_t)Le16(in) / 32768.0f;
                } else if (bytes_ == 3) {
                    v = (int32_t)(Le16(in) << 8 | (uint32_t)(uint8_t)in[2] << 24) / 2147483648.0f;
                } else {
                    v = (int32_t)Le32(in) / 2147483648.0f;
                }
                channels[c] = std::isfinite(v) ? v : 0.0f;
            }
            nativeaudio::Mix(channels, channels_, out + i * 2);
        }
    }

    std::shared_ptr<const Helper> helper_;
    std::wstring path_;
    std::mutex mx_;  // process_, between a restart on the decoding thread and Abort from another
    std::unique_ptr<HelperProcess> process_;
    std::vector<char> buffer_;
    std::string carry_;  // the start of a frame whose rest hasn't arrived
    unsigned rate_ = 0, channels_ = 0, bytes_ = 0;
    bool float_ = false, failed_ = false, ended_ = false;
    std::atomic<bool> aborted_{false};
    uint64_t skip_ = 0;
    double duration_ = 0;
    std::wstring problem_;
};

// The fields a helper's info command or a tool may print, under cordinal's names and ffprobe's.
enum class Field { None, Title, Artist, Album, AlbumArtist, Genre, Date, Composer, Comment, Grouping, Label, Lyrics,
                   Track, Disc, Bpm, Duration, SampleRate, Channels, BitRate, Kbps, RgTrackGain, RgTrackPeak,
                   RgAlbumGain, RgAlbumPeak };

Field FieldNamed(std::wstring key) {
    if (key.rfind(L"tag:", 0) == 0) key.erase(0, 4);  // ffprobe's "TAG:title="
    static const struct { const wchar_t* name; Field field; } names[] = {
        {L"title", Field::Title}, {L"artist", Field::Artist}, {L"album", Field::Album}, {L"album_artist", Field::AlbumArtist},
        {L"albumartist", Field::AlbumArtist}, {L"genre", Field::Genre}, {L"date", Field::Date}, {L"year", Field::Date},
        {L"composer", Field::Composer}, {L"comment", Field::Comment}, {L"description", Field::Comment},
        {L"grouping", Field::Grouping}, {L"label", Field::Label}, {L"publisher", Field::Label}, {L"lyrics", Field::Lyrics},
        {L"track", Field::Track}, {L"tracknumber", Field::Track}, {L"disc", Field::Disc}, {L"discnumber", Field::Disc},
        {L"bpm", Field::Bpm}, {L"duration", Field::Duration}, {L"length", Field::Duration},
        {L"sample_rate", Field::SampleRate}, {L"samplerate", Field::SampleRate}, {L"channels", Field::Channels},
        {L"bit_rate", Field::BitRate}, {L"bitrate", Field::Kbps}, {L"replaygain_track_gain", Field::RgTrackGain},
        {L"replaygain_track_peak", Field::RgTrackPeak}, {L"replaygain_album_gain", Field::RgAlbumGain},
        {L"replaygain_album_peak", Field::RgAlbumPeak}};
    for (const auto& n : names)
        if (key == n.name) return n.field;
    return Field::None;
}

// Calls f(key, value, line) for each line that isn't blank: its key lowercase with spaces as '_'
// (empty when the line has no '='), its value as the UTF-8 the program wrote.
void ForEachField(const std::string& text, const std::function<void(const std::wstring&, const std::string&, const std::string&)>& f) {
    for (size_t at = 0; at < text.size();) {
        size_t end = std::min(text.find('\n', at), text.size());
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        size_t eq = line.find('=');
        std::wstring key = eq == std::string::npos ? L"" : Lower(Trim(ProgramText(line.substr(0, eq))));
        std::replace(key.begin(), key.end(), L' ', L'_');
        std::string value = eq == std::string::npos ? "" : line.substr(eq + 1);
        value.erase(0, std::min(value.size(), value.find_first_not_of(" \t")));
        f(key, value, line);
    }
}

// "3" or "3/12"
void ParsePair(const std::string& value, uint32_t& number, uint32_t& total) {
    number = (uint32_t)strtoul(value.c_str(), nullptr, 10);
    size_t slash = value.find('/');
    if (slash != std::string::npos) total = (uint32_t)strtoul(value.c_str() + slash + 1, nullptr, 10);
}

}  // namespace

bool HelperCommand::Has(const wchar_t* placeholder) const {
    for (const auto& arg : args)
        for (const auto& name : Placeholders(arg))
            if (name == placeholder) return true;
    return false;
}

bool HelperCommand::UsesTrack() const {
    for (const auto& arg : args)
        for (const auto& name : Placeholders(arg))
            if (name == L"file" || IsTitleFormat(name)) return true;
    return false;
}

Helper ParseHelper(const std::wstring& id, const std::wstring& dir, const std::string& ini) {
    Helper h;
    h.id = h.name = id;
    h.dir = dir;
    const auto sections = ParseIni(ini, false);
    auto section = [&](const wchar_t* name) -> const IniSection* {
        for (const auto& s : sections)
            if (s.name == name) return &s;
        return nullptr;
    };
    auto text = [](const IniSection* s, const wchar_t* key) {
        const std::wstring* v = s ? s->Get(key) : nullptr;
        return v ? Trim(*v) : std::wstring();
    };
    const IniSection* about = section(L"helper");
    if (!about) {
        h.error = L"its helper.ini has no [helper] section";
        return h;
    }
    if (int format = _wtoi(text(about, L"format").c_str()); format > kHelperFormat) {
        h.error = L"it is written for a later cordinal (helper format " + std::to_wstring(format) + L")";
        return h;
    }
    if (!text(about, L"name").empty()) h.name = text(about, L"name");
    h.version = text(about, L"version");
    h.author = text(about, L"author");
    h.description = text(about, L"description");

    auto command = [&](const IniSection* s, const wchar_t* key, unsigned uses, const std::wstring& part, HelperCommand& out) {
        std::wstring line = text(s, key), why;
        if (line.empty()) return false;
        if (ParseCommand(line, dir, uses, out, why)) return true;
        h.warnings.push_back(part + L" is left out: " + why + L".");
        return false;
    };
    if (const IniSection* d = section(L"decoder")) {
        std::wstring list = text(d, L"extensions");
        std::replace_if(list.begin(), list.end(), [](wchar_t c) { return c == L',' || c == L';'; }, L' ');
        std::vector<std::wstring> extensions;
        for (std::wstring e : SplitWords(list)) {
            e = Lower(e);
            e.erase(0, std::min(e.size(), e.find_first_not_of(L"*.")));
            if (e.empty() || e.find_first_of(L"\\/:*?\"<>|.") != std::wstring::npos ||
                std::find(extensions.begin(), extensions.end(), e) != extensions.end())
                continue;
            if (IsNativeAudioType(e) || std::any_of(std::begin(kReserved), std::end(kReserved), [&](const wchar_t* r) { return e == r; }))
                h.warnings.push_back(L"." + e + L" files are cordinal's own; the helper isn't used for them.");
            else extensions.push_back(e);
        }
        if (extensions.empty()) h.warnings.push_back(L"The decoder is left out: it names no file type for cordinal to give it.");
        else if (command(d, L"decode", kFile | kStart, L"The decoder", h.decode) && !h.decode.Has(L"file")) {
            h.warnings.push_back(L"The decoder is left out: its decode command doesn't say where the file goes ({file}).");
            h.decode = {};
        }
        if (!h.decode.program.empty()) {
            h.extensions = std::move(extensions);
            if (command(d, L"info", kFile, L"Reading tags", h.info) && !h.info.Has(L"file")) {
                h.warnings.push_back(L"Reading tags is left out: its info command doesn't say where the file goes ({file}).");
                h.info = {};
            }
        }
    }
    for (const auto& s : sections) {
        const bool tool = s.name.rfind(L"tool", 0) == 0, visualizer = s.name.rfind(L"visualizer", 0) == 0;
        if (!tool && !visualizer) continue;
        std::wstring label = text(&s, L"label");
        if (label.empty()) {
            h.warnings.push_back(tool ? L"A tool without a label is left out." : L"A visualizer without a label is left out.");
            continue;
        }
        if (tool) {
            HelperTool t;
            t.label = label;
            if (h.tools.size() >= kMaxTools || !command(&s, L"command", kFile | kFiles | kFormat, L"The tool " + label, t.command)) continue;
            std::wstring output = Lower(text(&s, L"output"));
            t.output = output == L"tags" ? HelperTool::Tags : output == L"nothing" || output == L"none" ? HelperTool::Nothing : HelperTool::Text;
            t.together = t.command.Has(L"files");
            t.perTrack = !t.together && t.command.UsesTrack();
            if (t.output == HelperTool::Tags && !t.perTrack)
                h.warnings.push_back(L"The tool " + label + L" is left out: its tags must come from one run per track ({file}).");
            else h.tools.push_back(std::move(t));
        } else {
            HelperVisualizer v;
            v.label = label;
            if (h.visualizers.size() >= kMaxVisualizers || !command(&s, L"command", kFeed, L"The visualizer " + label, v.command)) continue;
            if (!v.command.Has(L"feed"))
                h.warnings.push_back(L"The visualizer " + label + L" is left out: its command doesn't take the feed ({feed}).");
            else h.visualizers.push_back(std::move(v));
        }
    }
    auto use = [&](const HelperCommand& c) {
        if (!c.program.empty() &&
            std::none_of(h.programs.begin(), h.programs.end(), [&](auto& p) { return _wcsicmp(p.c_str(), c.program.c_str()) == 0; }))
            h.programs.push_back(c.program);
    };
    use(h.decode);
    use(h.info);
    for (const auto& t : h.tools) use(t.command);
    for (const auto& v : h.visualizers) use(v.command);
    if (h.programs.empty()) h.error = h.warnings.empty() ? L"its helper.ini doesn't say what it does" : L"none of it can be used";
    return h;
}

std::vector<Helper> ReadHelpers(const std::wstring& root) {
    std::vector<std::wstring> ids;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((root + kSlashStar).c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && fd.cFileName[0] != L'~')
                ids.push_back(fd.cFileName);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    SortObjects(ids.begin(), ids.end(), [](auto& a, auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<Helper> helpers;
    for (const auto& id : ids) {
        const std::wstring dir = root + kSlash + id;
        std::string ini;
        if (id.find_first_of(L"=[];|*") == std::wstring::npos && ReadSmallFile(dir + kSlash + L"helper.ini", kMaxManifest, ini)) {
            helpers.push_back(ParseHelper(id, dir, ini));
            continue;
        }
        Helper h;
        h.id = h.name = id;
        h.dir = dir;
        h.error = id.find_first_of(L"=[];|*") != std::wstring::npos ? L"its folder name has one of = [ ] ; | *, which settings.ini can't keep"
                                                                    : L"its folder has no helper.ini of at most 64 KB";
        helpers.push_back(std::move(h));
    }
    return helpers;
}

std::wstring HelperSummary(const Helper& h) {
    std::vector<std::wstring> parts, types, tools, visualizers;
    for (const auto& e : h.extensions) types.push_back(L"." + e);
    for (const auto& t : h.tools) tools.push_back(t.label);
    for (const auto& v : h.visualizers) visualizers.push_back(v.label);
    if (!types.empty()) parts.push_back(L"plays " + JoinWords(types) + L" files" + (h.info.program.empty() ? L"" : L" and reads their tags"));
    if (!tools.empty()) parts.push_back(std::wstring(tools.size() == 1 ? L"adds the tool " : L"adds the tools ") + JoinWords(tools));
    if (!visualizers.empty())
        parts.push_back(std::wstring(visualizers.size() == 1 ? L"shows the visualizer " : L"shows the visualizers ") + JoinWords(visualizers));
    return parts.empty() ? L"does nothing cordinal can use" : JoinWords(parts);
}

std::wstring HelperProgramList(const Helper& h) { return JoinWords(h.programs); }

std::wstring HelpersOffReason(bool safe, bool elevated) {
    if (safe) return L"off in safe mode";
    if (elevated) return L"off while cordinal runs as administrator";
    return L"";
}

#ifdef _WIN32
bool ProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

#else
bool ProcessElevated() { return geteuid() == 0; }  // root would hand its rights to programs in a folder the user can change
#endif

std::wstring HelperArguments(const HelperCommand& c, const HelperValues& v) {
    std::wstring line;
    auto add = [&](const std::wstring& arg) { line += (line.empty() ? L"" : L" ") + Argument(arg); };
    for (const auto& arg : c.args) {
        if (arg == L"{files}") {
            for (const auto& f : v.files) add(f);
            continue;
        }
        std::wstring out;
        for (size_t i = 0; i < arg.size(); ++i) {
            const wchar_t ch = arg[i];
            size_t close = ch == L'{' ? arg.find(L'}', i) : std::wstring::npos;
            if ((ch == L'{' || ch == L'}') && i + 1 < arg.size() && arg[i + 1] == ch) {
                out += ch;  // "{{" and "}}" stand for the braces themselves
                ++i;
            } else if (close == std::wstring::npos) {
                out += ch;
            } else {
                std::wstring name = arg.substr(i + 1, close - i - 1);
                if (name == L"file") out += v.file;
                else if (name == L"start") out += FormatSeconds(v.start);
                else if (name == L"feed") out += v.feed;
                else if (v.format) out += v.format(name);
                i = close;
            }
        }
        add(out);
    }
    return line;
}

#ifdef _WIN32
// --- HelperProcess ---

HelperProcess::~HelperProcess() {
    if (Running()) Stop();
    for (HANDLE h : {process_, out_, err_})
        if (h) CloseHandle(h);
}

bool HelperProcess::Start(const std::wstring& program, const std::wstring& arguments, const std::wstring& dir, std::wstring& error,
                          HANDLE inherit, bool window) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outWrite = nullptr, errWrite = nullptr, nul = INVALID_HANDLE_VALUE;
    std::vector<HANDLE> handles;
    DWORD code = 0;
    if (!window) {
        if (CreatePipe(&out_, &outWrite, &sa, 1 << 20) && CreatePipe(&err_, &errWrite, &sa, 1 << 16)) {
            SetHandleInformation(out_, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(err_, HANDLE_FLAG_INHERIT, 0);
            nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        }
        if (nul == INVALID_HANDLE_VALUE) code = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
        else handles = {nul, outWrite, errWrite};
    }
    if (inherit) handles.push_back(inherit);
    // Only these handles reach the program: a pipe of another helper starting at the same time must
    // never be held open by this one, or that helper's output would never end.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> list(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.data());
    const bool listed = !code && !handles.empty() && InitializeProcThreadAttributeList(attributes, 1, 0, &size);
    PROCESS_INFORMATION pi{};
    if (!code) {
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        if (!window) {
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = nul;
            si.StartupInfo.hStdOutput = outWrite;
            si.StartupInfo.hStdError = errWrite;
        }
        if (listed && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                                handles.size() * sizeof(HANDLE), nullptr, nullptr))
            si.lpAttributeList = attributes;
        // A program started for playback keeps up with it; one started for a background scan stays
        // behind the music.
        const DWORD flags = CREATE_SUSPENDED | (si.lpAttributeList ? EXTENDED_STARTUPINFO_PRESENT : 0) | (window ? 0 : CREATE_NO_WINDOW) |
                            (GetThreadPriority(GetCurrentThread()) < THREAD_PRIORITY_NORMAL ? BELOW_NORMAL_PRIORITY_CLASS : 0);
        std::wstring command = QuoteArg(program) + (arguments.empty() ? L"" : L" " + arguments);
        if (!handles.empty() && !si.lpAttributeList) code = ERROR_INVALID_HANDLE;
        else if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, si.lpAttributeList != nullptr, flags, nullptr,
                                 dir.c_str(), &si.StartupInfo, &pi))
            code = GetLastError();
    }
    if (listed) DeleteProcThreadAttributeList(attributes);
    for (HANDLE h : {outWrite, errWrite})
        if (h) CloseHandle(h);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (code || !pi.hProcess) {
        error = SystemText(code ? code : ERROR_GEN_FAILURE);
        for (HANDLE* h : {&out_, &err_})
            if (*h) {
                CloseHandle(*h);
                *h = nullptr;
            }
        return false;
    }
    if (HANDLE job = Job()) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    return true;
}

void HelperProcess::DrainErrors() {
    DWORD avail = 0;
    while (err_ && PeekNamedPipe(err_, nullptr, 0, nullptr, &avail, nullptr) && avail) {
        char buffer[4096];
        DWORD got = 0;
        if (!ReadFile(err_, buffer, std::min<DWORD>(avail, sizeof(buffer)), &got, nullptr) || !got) break;
        errors_.append(buffer, got);
        if (errors_.size() > 8192) errors_.erase(0, errors_.size() - 4096);
    }
}

size_t HelperProcess::Read(void* buffer, size_t bytes, ULONGLONG deadline, const std::atomic<bool>* cancel) {
    if (!out_ || !bytes) return 0;
    for (;;) {
        if (cancel && *cancel) Stop();
        if (stopped_) return 0;
        DrainErrors();  // a program blocked on a full stderr pipe would never write the rest
        DWORD avail = 0;
        if (!PeekNamedPipe(out_, nullptr, 0, nullptr, &avail, nullptr)) return 0;  // it closed its output, all of it read
        if (avail) {
            DWORD got = 0;
            return ReadFile(out_, buffer, (DWORD)std::min<size_t>(bytes, avail), &got, nullptr) ? got : 0;
        }
        // A program it started may still hold the pipe open, so the program's own end is the end of
        // its output once the pipe is empty.
        if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
            if (PeekNamedPipe(out_, nullptr, 0, nullptr, &avail, nullptr) && avail) continue;
            return 0;
        }
        if (deadline && GetTickCount64() >= deadline) {
            timedOut_ = true;
            Stop();
            return 0;
        }
        Sleep(2);
    }
}

bool HelperProcess::ReadAll(std::string& out, size_t limit, const std::atomic<bool>* cancel) {
    char buffer[16384];
    for (;;) {
        // A tool may think for minutes before it prints: it is stopped by cancelling, not by a timeout.
        size_t n = Read(buffer, sizeof(buffer), cancel ? 0 : GetTickCount64() + HelperPatience(), cancel);
        if (!n) return !timedOut_ && !stopped_;
        if (out.size() + n > limit) {
            Stop();
            return false;
        }
        out.append(buffer, n);
    }
}

DWORD HelperProcess::Finish(DWORD ms) {
    if (!process_) return ERROR_GEN_FAILURE;
    if (WaitForSingleObject(process_, ms) != WAIT_OBJECT_0) {
        Stop();
        WaitForSingleObject(process_, 1000);
        return ERROR_TIMEOUT;
    }
    DWORD code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(process_, &code);
    DrainErrors();
    return stopped_ && !code ? ERROR_CANCELLED : code;
}

void HelperProcess::Stop() {
    stopped_ = true;
    if (process_) TerminateProcess(process_, ERROR_CANCELLED);
}

bool HelperProcess::Running() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

std::wstring HelperProcess::Complaint() {
    DrainErrors();
    std::string text = errors_;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    size_t line = text.find_last_of('\n');
    std::wstring last = Trim(ProgramText(line == std::string::npos ? text : text.substr(line + 1)));
    if (last.size() > 200) last = last.substr(0, 199) + L"\x2026";
    while (!last.empty() && last.back() == L'.') last.pop_back();
    return last;
}

void StopHelperPrograms() {
    if (HANDLE job = Job()) TerminateJobObject(job, ERROR_CANCELLED);
}

#else
// --- HelperProcess (Linux) ---

// A command line as the argument vector it stands for: the rules CommandLineToArgvW reverses, which is what
// QuoteArg writes, so every value cordinal puts in reaches the program as one argument.
std::vector<std::string> ArgvFromCommandLine(const std::wstring& line) {
    std::vector<std::string> argv;
    std::wstring word;
    bool quoted = false, any = false;
    for (size_t i = 0; i < line.size(); ++i) {
        wchar_t c = line[i];
        if (c == L'\\') {
            size_t slashes = 0;
            while (i < line.size() && line[i] == L'\\') ++slashes, ++i;
            if (i < line.size() && line[i] == L'"') {
                word.append(slashes / 2, L'\\');
                if (slashes % 2) {
                    word += L'"';
                    any = true;
                    continue;
                }
                --i;  // an even run: the quote that follows is a delimiter
                any = true;
                continue;
            }
            word.append(slashes, L'\\');
            any = true;
            --i;
        } else if (c == L'"') {
            quoted = !quoted;
            any = true;
        } else if ((c == L' ' || c == L'\t') && !quoted) {
            if (any) argv.push_back(ToUtf8(word));
            word.clear();
            any = false;
        } else {
            word += c;
            any = true;
        }
    }
    if (any) argv.push_back(ToUtf8(word));
    return argv;
}

std::mutex g_pidMx;
std::vector<pid_t> g_pids;  // the process groups of the programs running, so closing cordinal ends them

constexpr DWORD kCancelled = 1223, kTimeout = 1460, kGeneral = 31;

HelperProcess::~HelperProcess() {
    if (Running()) Stop();
    if (pid_ > 0) {
        int status = 0;
        waitpid(pid_, &status, 0);
        std::lock_guard<std::mutex> lock(g_pidMx);
        std::erase(g_pids, pid_);
    }
    for (int fd : {out_, err_})
        if (fd >= 0) close(fd);
}

bool HelperProcess::Start(const std::wstring& program, const std::wstring& arguments, const std::wstring& dir, std::wstring& error,
                          HANDLE, bool) {
    int outPipe[2], errPipe[2];
    if (pipe2(outPipe, O_CLOEXEC) != 0) return error = SystemText((DWORD)errno), false;
    if (pipe2(errPipe, O_CLOEXEC) != 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        return error = SystemText((DWORD)errno), false;
    }
    std::vector<std::string> args = ArgvFromCommandLine(arguments);
    const std::string path = ToUtf8(program), cwd = ToUtf8(dir);
    std::vector<char*> argv{const_cast<char*>(path.c_str())};
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], 2);
    posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);  // its own group: Stop ends it and whatever it started
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t pid = 0;
    int result = posix_spawn(&pid, path.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(outPipe[1]);
    close(errPipe[1]);
    if (result != 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        error = SystemText((DWORD)result);
        return false;
    }
    out_ = outPipe[0];
    err_ = errPipe[0];
    pid_ = pid;
    fcntl(err_, F_SETFL, fcntl(err_, F_GETFL) | O_NONBLOCK);
    std::lock_guard<std::mutex> lock(g_pidMx);
    g_pids.push_back(pid);
    return true;
}

void HelperProcess::DrainErrors() {
    char buffer[4096];
    for (ssize_t got; err_ >= 0 && (got = read(err_, buffer, sizeof(buffer))) > 0;) {
        errors_.append(buffer, (size_t)got);
        if (errors_.size() > 8192) errors_.erase(0, errors_.size() - 4096);
    }
}

// Whether the program has exited, reaping it once it has.
bool HelperProcess::Exited() const {
    if (exited_) return true;
    if (pid_ <= 0) return true;
    int status = 0;
    pid_t done = waitpid(pid_, &status, WNOHANG);
    if (done == 0) return false;
    exited_ = true;
    status_ = done < 0 ? 0 : WIFEXITED(status) ? (DWORD)WEXITSTATUS(status) : (DWORD)(128 + WTERMSIG(status));
    return true;
}

size_t HelperProcess::Read(void* buffer, size_t bytes, ULONGLONG deadline, const std::atomic<bool>* cancel) {
    if (out_ < 0 || !bytes) return 0;
    for (;;) {
        if (cancel && *cancel) Stop();
        if (stopped_) return 0;
        DrainErrors();  // a program blocked on a full stderr pipe would never write the rest
        pollfd pfd{out_, POLLIN, 0};
        int ready = poll(&pfd, 1, 20);
        if (ready > 0) {
            ssize_t got = read(out_, buffer, bytes);
            if (got > 0) return (size_t)got;
            if (got == 0 || errno != EINTR) return 0;  // it closed its output, all of it read
            continue;
        }
        // A program it started may still hold the pipe open, so the program's own end is the end of
        // its output once the pipe is empty.
        if (Exited()) return 0;
        if (deadline && GetTickCount64() >= deadline) {
            timedOut_ = true;
            Stop();
            return 0;
        }
    }
}

bool HelperProcess::ReadAll(std::string& out, size_t limit, const std::atomic<bool>* cancel) {
    char buffer[16384];
    for (;;) {
        // A tool may think for minutes before it prints: it is stopped by cancelling, not by a timeout.
        size_t n = Read(buffer, sizeof(buffer), cancel ? 0 : GetTickCount64() + HelperPatience(), cancel);
        if (!n) return !timedOut_ && !stopped_;
        if (out.size() + n > limit) {
            Stop();
            return false;
        }
        out.append(buffer, n);
    }
}

DWORD HelperProcess::Finish(DWORD ms) {
    if (pid_ <= 0) return kGeneral;
    const ULONGLONG until = GetTickCount64() + ms;
    while (!Exited() && GetTickCount64() < until) usleep(2000);
    if (!Exited()) {
        Stop();
        for (int i = 0; i < 500 && !Exited(); ++i) usleep(2000);
        return kTimeout;
    }
    DrainErrors();
    return stopped_ && !status_ ? kCancelled : status_;
}

void HelperProcess::Stop() {
    stopped_ = true;
    if (pid_ > 0 && !exited_) {
        killpg(pid_, SIGKILL);
        kill(pid_, SIGKILL);
    }
}

bool HelperProcess::Running() const { return pid_ > 0 && !Exited(); }

std::wstring HelperProcess::Complaint() {
    DrainErrors();
    std::string text = errors_;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    size_t line = text.find_last_of('\n');
    std::wstring last = Trim(ProgramText(line == std::string::npos ? text : text.substr(line + 1)));
    if (last.size() > 200) last = last.substr(0, 199) + L"\x2026";
    while (!last.empty() && last.back() == L'.') last.pop_back();
    return last;
}

void StopHelperPrograms() {
    std::lock_guard<std::mutex> lock(g_pidMx);
    for (pid_t pid : g_pids) {
        killpg(pid, SIGKILL);
        kill(pid, SIGKILL);
    }
}
#endif

void SetHelperPatience(unsigned milliseconds) { g_patience = milliseconds; }
unsigned HelperPatience() { return g_patience; }

// --- Trust ---

std::wstring MakeHelperTrust(const std::wstring& dir) {
    auto files = TrustedFiles(dir);
    for (auto& f : files)
        if ((f.hash = Sha256(dir + kSlash + f.name)).empty()) return L"";
    return files.empty() ? L"" : TrustRecord(files);
}

bool CheckHelperTrust(const std::wstring& dir, std::wstring& record) {
    auto known = ParseTrust(record), now = TrustedFiles(dir);
    if (known.empty() || known.size() != now.size()) return false;
    bool touched = false;
    for (size_t i = 0; i < now.size(); ++i) {
        if (_wcsicmp(known[i].name.c_str(), now[i].name.c_str())) return false;
        now[i].hash = known[i].hash;
        if (known[i].size == now[i].size && known[i].time == now[i].time) continue;
        if (Sha256(dir + kSlash + now[i].name) != known[i].hash) return false;
        touched = true;  // saved again or copied with a new time: the same bytes
    }
    if (touched) record = TrustRecord(now);
    return true;
}

// --- The helpers that are on ---

void SetActiveHelpers(std::vector<std::shared_ptr<const Helper>> on) {
    auto next = std::make_shared<const std::vector<std::shared_ptr<const Helper>>>(std::move(on));
    std::lock_guard<std::mutex> lock(g_activeMx);
    g_active = std::move(next);
}

std::shared_ptr<const Helper> HelperForExtension(const std::wstring& ext) {
    auto on = ext.empty() ? nullptr : ActiveHelpers();
    if (on)
        for (const auto& h : *on)
            if (std::find(h->extensions.begin(), h->extensions.end(), ext) != h->extensions.end()) return h;
    return nullptr;
}

std::wstring HelperFileTypes() {
    std::wstring types;
    if (auto on = ActiveHelpers())
        for (const auto& h : *on)
            for (const auto& e : h->extensions) types += L";*." + e;
    return types;
}

std::unique_ptr<Decoder> OpenHelperDecoder(const std::wstring& path, std::wstring& error) {
    auto helper = HelperForExtension(ExtOf(path));
    if (!helper) {
        error = L"Couldn't play " + FileNameOf(path) + L": no helper that is on plays this file type.";
        return nullptr;
    }
    auto d = std::make_unique<HelperDecoder>(helper, path);
    std::wstring why;
    if (!d->Start(0, why)) {
        error = L"Couldn't play " + FileNameOf(path) + L": " + why + L".";
        return nullptr;
    }
    return d;
}

bool ReadHelperTags(const std::wstring& path, TagData& out) {
    out = TagData{};
    auto helper = HelperForExtension(ExtOf(path));
    if (!helper) return false;
    if (helper->info.program.empty()) return true;
    HelperValues v;
    v.file = path;
    HelperProcess p;
    std::wstring error;
    std::string text;
    if (!p.Start(helper->dir + kSlash + helper->info.program, HelperArguments(helper->info, v), helper->dir, error) ||
        !p.ReadAll(text, 1 << 20))
        return false;
    const DWORD code = p.Finish(3000);  // one that closed its output but lingers has printed all of it
    if (code != 0 && code != ERROR_TIMEOUT) return false;
    ParseHelperInfo(text, out);
    return true;
}

std::wstring HelperText(const std::string& bytes) { return ProgramText(bytes); }

void ParseHelperInfo(const std::string& text, TagData& out) {
    ForEachField(text, [&](const std::wstring& key, const std::string& value, const std::string&) {
        if (value.empty()) return;
        auto set = [&](std::string& field) {
            if (field.empty()) field = value;
        };
        auto count = [&](uint32_t& field) {
            if (!field) field = (uint32_t)strtoul(value.c_str(), nullptr, 10);
        };
        switch (FieldNamed(key)) {
            case Field::Title: set(out.title); break;
            case Field::Artist: set(out.artist); break;
            case Field::Album: set(out.album); break;
            case Field::AlbumArtist: set(out.albumArtist); break;
            case Field::Genre: set(out.genre); break;
            case Field::Date: set(out.date); break;
            case Field::Composer: set(out.composer); break;
            case Field::Comment: set(out.comment); break;
            case Field::Grouping: set(out.grouping); break;
            case Field::Label: set(out.label); break;
            case Field::Track: if (!out.track) ParsePair(value, out.track, out.trackTotal); break;
            case Field::Disc: if (!out.disc) ParsePair(value, out.disc, out.discTotal); break;
            case Field::Bpm: count(out.bpm); break;
            case Field::SampleRate: count(out.sampleRate); break;
            case Field::Channels: count(out.channels); break;
            case Field::Kbps: count(out.bitrate); break;
            case Field::BitRate: if (!out.bitrate) out.bitrate = (uint32_t)(strtoull(value.c_str(), nullptr, 10) / 1000); break;
            case Field::Duration: {
                double seconds = strtod(value.c_str(), nullptr);
                if (!out.duration && std::isfinite(seconds) && seconds > 0) out.duration = seconds;
                break;
            }
            default: break;
        }
    });
}

void ParseHelperEdit(const std::string& text, TagEdit& edit, std::vector<std::wstring>& ignored) {
    ForEachField(text, [&](const std::wstring& key, const std::string& value, const std::string& line) {
        auto number = [&](std::optional<uint32_t>& field, std::optional<uint32_t>* total) {
            uint32_t n = 0, of = 0;
            ParsePair(value, n, of);
            field = n;
            if (total && value.find('/') != std::string::npos) *total = of;
        };
        switch (FieldNamed(key)) {
            case Field::Title: edit.title = value; break;
            case Field::Artist: edit.artist = value; break;
            case Field::Album: edit.album = value; break;
            case Field::AlbumArtist: edit.albumArtist = value; break;
            case Field::Genre: edit.genre = value; break;
            case Field::Date: edit.date = value; break;
            case Field::Composer: edit.composer = value; break;
            case Field::Comment: edit.comment = value; break;
            case Field::Grouping: edit.grouping = value; break;
            case Field::Label: edit.label = value; break;
            case Field::Lyrics: edit.lyrics = value; break;
            case Field::Track: number(edit.track, &edit.trackTotal); break;
            case Field::Disc: number(edit.disc, &edit.discTotal); break;
            case Field::Bpm: number(edit.bpm, nullptr); break;
            case Field::RgTrackGain: edit.rgTrackGain = value; break;
            case Field::RgTrackPeak: edit.rgTrackPeak = value; break;
            case Field::RgAlbumGain: edit.rgAlbumGain = value; break;
            case Field::RgAlbumPeak: edit.rgAlbumPeak = value; break;
            default: ignored.push_back(ProgramText(line)); break;
        }
    });
}

// --- The visual feed ---

#ifdef _WIN32
bool HelperFeedWriter::Open() {
    if (view_) return true;
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(HelperFeed), nullptr);
    if (mapping_) view_ = static_cast<HelperFeed*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(HelperFeed)));
    if (!view_) {
        Close();
        return false;
    }
    std::memcpy(view_->magic, "CDVF", 4);  // a new mapping is all zeros: stopped, at sequence 0
    view_->version = 1;
    view_->size = sizeof(HelperFeed);
    view_->frames = HelperFeed::kFrames;
    view_->bands = HelperFeed::kBands;
    view_->clock = -1;
    return true;
}

HANDLE HelperFeedWriter::ReaderHandle() const {
    HANDLE reader = nullptr;
    if (mapping_) DuplicateHandle(GetCurrentProcess(), mapping_, GetCurrentProcess(), &reader, FILE_MAP_READ, TRUE, 0);
    return reader;
}

void HelperFeedWriter::Publish(const std::function<void(HelperFeed& block)>& fill) {
    if (!view_) return;
    auto next = std::make_unique<HelperFeed>(*view_);
    fill(*next);
    // Odd while the block changes: a reader that sees it odd, or sees it change, reads again.
    InterlockedIncrement(&view_->sequence);
    const size_t from = offsetof(HelperFeed, state);
    std::memcpy(reinterpret_cast<char*>(view_) + from, reinterpret_cast<const char*>(next.get()) + from, sizeof(HelperFeed) - from);
    InterlockedIncrement(&view_->sequence);
}

void HelperFeedWriter::Close() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
    view_ = nullptr;
    mapping_ = nullptr;
}

bool ReadHelperFeed(const HelperFeed* shared, HelperFeed& copy) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        const LONG before = shared->sequence;
        MemoryBarrier();
        if (before & 1) {
            YieldProcessor();
            continue;
        }
        std::memcpy(&copy, (const void*)shared, sizeof(HelperFeed));
        MemoryBarrier();
        if (shared->sequence == before) return true;
    }
    return false;
}

#endif
