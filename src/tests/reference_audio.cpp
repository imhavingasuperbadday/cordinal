// Independent test oracle. Never compiled or linked into the player.
// Provides a Decoder that streams raw f32le from an ffmpeg child process, and a direct MP4 parser
// (no ffmpeg) for the gapless skip/valid fields, so tests have a reference to compare against and a
// way to inspect containers the player itself does not decode.
#include "reference_audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

// The parent_path()/operator/ of std::filesystem, done with plain strings so this oracle has no
// dependency on the player's path helpers.
std::wstring ParentDir(const std::wstring& p) {
    size_t slash = p.find_last_of(L"\\/");
    size_t drive = p.size() >= 2 && p[1] == L':' ? 2 : 0;
    if (slash == std::wstring::npos) return p.substr(0, drive);
    return p.substr(0, slash == 0 || slash == drive ? slash + 1 : slash);
}

// A directory plus a leaf name, adding a separator unless the directory already ends in one.
std::wstring JoinPath(const std::wstring& dir, const wchar_t* name) {
    if (dir.empty()) return name;
    std::wstring r = dir;
    if (r.back() != L'\\' && r.back() != L'/' && !(r.size() == 2 && r[1] == L':')) r += L'\\';
    r += name;
    return r;
}

// True only for a regular file (missing entries and directories both return false).
bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Calls f(name) for each entry in dir (never "." or ".."), like std::filesystem's iterator.
template <class F>
void ForEachEntry(const std::wstring& dir, F&& f) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t* name = fd.cFileName;
        if (name[0] == L'.' && (!name[1] || (name[1] == L'.' && !name[2]))) continue;
        f(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Reads an environment variable into a std::wstring, or returns empty if it is unset/too long.
std::wstring Env(const wchar_t* name) {
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)std::size(buf));
    return n && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

// Accepts a candidate ffmpeg only if a sibling ffprobe.exe exists, so both halves of the oracle
// are present before any test relies on them.
bool ToolsAt(const std::wstring& ffmpeg, FFmpegTools& out) {
    if (!FileExists(ffmpeg)) return false;
    std::wstring probe = JoinPath(ParentDir(ffmpeg), L"ffprobe.exe");
    if (!FileExists(probe)) return false;
    out = {ffmpeg, probe};
    return true;
}

// A shared job object with KILL_ON_JOB_CLOSE: any ffmpeg child (and its own children) is killed
// automatically if this test process goes away, so a hung decoder can never linger.
HANDLE Job() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        return j;
    }();
    return job;
}

}  // namespace

// Searched once, in order: CORDINAL_FFMPEG, the legacy WAVEFORGE_FFMPEG, ffmpeg.exe next to the
// exe, ffmpeg.exe on PATH, Program Files, then the WinGet install location. Returns null if none
// had a matching ffprobe.
const FFmpegTools* FindFFmpeg() {
    static FFmpegTools tools;
    static bool found = false;
    static std::once_flag once;
    std::call_once(once, [] {
        std::vector<std::wstring> candidates;
        if (auto custom = Env(L"CORDINAL_FFMPEG"); !custom.empty()) candidates.push_back(custom);
        if (auto legacy = Env(L"WAVEFORGE_FFMPEG"); !legacy.empty()) candidates.push_back(legacy);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        candidates.push_back(JoinPath(ParentDir(exe), L"ffmpeg.exe"));
        wchar_t onPath[MAX_PATH];
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, onPath, nullptr)) candidates.push_back(onPath);
        if (auto pf = Env(L"ProgramFiles"); !pf.empty()) candidates.push_back(pf + L"\\ffmpeg\\bin\\ffmpeg.exe");
        if (auto local = Env(L"LOCALAPPDATA"); !local.empty()) {
            candidates.push_back(local + L"\\Microsoft\\WinGet\\Links\\ffmpeg.exe");
            std::wstring packages = local + L"\\Microsoft\\WinGet\\Packages";
            ForEachEntry(packages, [&](const wchar_t* pkg) {
                if (std::wstring(pkg).rfind(L"Gyan.FFmpeg", 0) != 0) return;
                ForEachEntry(packages + L"\\" + pkg, [&](const wchar_t* sub) {
                    candidates.push_back(packages + L"\\" + pkg + L"\\" + sub + L"\\bin\\ffmpeg.exe");
                });
            });
        }
        for (auto& c : candidates)
            if (ToolsAt(c, tools)) {
                found = true;
                break;
            }
    });
    return found ? &tools : nullptr;
}

// Kills the child and releases the read pipe and process handle.
ChildProcess::~ChildProcess() {
    Kill();
    if (out_) CloseHandle(out_);
    if (process_) CloseHandle(process_);
}

// Creates the child suspended on a 1 MiB stdout pipe, puts it in the kill-on-close job, then
// resumes it; stdin/stderr go to NUL so nothing can block on a console.
bool ChildProcess::Start(const std::wstring& exe, const std::wstring& args) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 1 << 20)) return false;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = QuoteArg(exe) + L" " + args;
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr,
                             &si, &pi);
    CloseHandle(write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(read);
        return false;
    }
    if (HANDLE job = Job()) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    out_ = read;
    return true;
}

size_t ChildProcess::Read(void* buffer, size_t bytes) {
    if (!out_) return 0;
    DWORD got = 0;
    if (!ReadFile(out_, buffer, (DWORD)bytes, &got, nullptr)) return 0;
    return got;
}

std::string ChildProcess::ReadAll() {
    std::string all;
    char buf[8192];
    while (size_t n = Read(buf, sizeof(buf))) all.append(buf, n);
    return all;
}

void ChildProcess::Kill() {
    if (process_) TerminateProcess(process_, 1);
}

namespace {
// Big-endian integer readers for the MP4 box parser.
uint32_t BE32(const unsigned char* p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
uint64_t BE64(const unsigned char* p) { return (uint64_t)BE32(p)<<32 | BE32(p+4); }
// Decoder over ffmpeg's stdout: reads raw f32le stereo and buffers the part of a 8-byte (one
// interleaved frame) that a short pipe read split, so every returned frame is whole.
class FFmpegDecoder final : public Decoder {
public:
    bool Open(const std::wstring& path, unsigned rate, double duration) {
        path_ = path;
        rate_ = rate;
        duration_ = duration;
        return Start(0);
    }
    size_t Read(float* out, size_t frames) override {
        if (!process_) return 0;
        size_t want = frames * 2 * sizeof(float);
        auto* bytes = reinterpret_cast<char*>(out);
        size_t got = carry_;
        memcpy(bytes, carryBytes_, carry_);
        carry_ = 0;
        while (got < want) {
            size_t n = process_->Read(bytes + got, want - got);
            if (!n) break;
            got += n;
        }
        size_t whole = got / 8 * 8;
        carry_ = got - whole;
        memcpy(carryBytes_, bytes + whole, carry_);
        return whole / 8;
    }
    bool Seek(double seconds) override { return Start(std::max(0.0, seconds)); }
    double Duration() const override { return duration_; }
    unsigned Rate() const override { return rate_; }
    void Abort() override {
        if (process_) process_->Kill();
    }

private:
    bool Start(double at) {
        process_.reset();
        carry_ = 0;
        const FFmpegTools* tools = FindFFmpeg();
        if (!tools) return false;
        wchar_t seek[64];
        swprintf_s(seek, L"-ss %.3f ", at);
        std::wstring args = L"-hide_banner -loglevel error -nostdin " + std::wstring(at > 0 ? seek : L"") + L"-i " +
                            QuoteArg(path_) + L" -map 0:a:0 -vn -sn -dn -ac 2 -ar " + std::to_wstring(rate_) +
                            L" -f f32le -";
        auto p = std::make_unique<ChildProcess>();
        if (!p->Start(tools->ffmpeg, args)) return false;
        process_ = std::move(p);
        return true;
    }
    std::wstring path_;
    unsigned rate_ = 48000;
    double duration_ = 0;
    std::unique_ptr<ChildProcess> process_;
    char carryBytes_[8] = {};
    size_t carry_ = 0;
};

}

// Opens a reference decode of `path` at `rate` via ffmpeg, or null if ffmpeg is unavailable/fails.
std::unique_ptr<Decoder> OpenFFmpegDecoder(const std::wstring& path, unsigned rate) {
    auto d = std::make_unique<FFmpegDecoder>();
    if (!d->Open(path,rate,0)) return nullptr;
    return d;
}

// Reads the MP4 gapless skip/valid pair without ffmpeg. It walks the top-level boxes to the moov
// atom, then prefers the iTunes iTunSMPB tag and falls back to the audio track's edit list
// (media_time = priming, segment_duration = valid length).
bool ReadMp4Gapless(const std::wstring& path, GaplessInfo& info) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    auto readAt = [&](uint64_t off, size_t n) {
        std::vector<unsigned char> b(n);
        OVERLAPPED ov{};
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        if (!ReadFile(h, b.data(), (DWORD)n, &got, &ov)) got = 0;
        b.resize(got);
        return b;
    };
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    uint64_t pos = 0, fileSize = (uint64_t)size.QuadPart;
    std::vector<unsigned char> moov;
    while (pos + 8 <= fileSize) {
        auto hd = readAt(pos, 16);
        if (hd.size() < 8) break;
        uint64_t box = BE32(hd.data());
        size_t hl = 8;
        if (box == 1 && hd.size() >= 16) { box = BE64(&hd[8]); hl = 16; }
        else if (box == 0) box = fileSize - pos;
        if (box < hl) break;
        if (!memcmp(&hd[4], "moov", 4)) {
            if (box < (64u << 20)) moov = readAt(pos + hl, (size_t)(box - hl));
            break;
        }
        pos += box;
    }
    CloseHandle(h);
    if (moov.empty()) return false;

    static const char key[] = "iTunSMPB";
    auto it = std::search(moov.begin(), moov.end(), key, key + 8);
    if (it != moov.end()) {
        static const char data[] = "data";
        auto d = std::search(it, moov.end(), data, data + 4);
        if (d != moov.end() && d - moov.begin() >= 4) {
            size_t atom = (size_t)(d - moov.begin()) - 4;
            uint32_t len = BE32(&moov[atom]);
            if (len > 16 && atom + len <= moov.size()) {
                std::string text(moov.begin() + atom + 16, moov.begin() + atom + len);
                unsigned long long f[4] = {};
                if (sscanf_s(text.c_str(), " %llx %llx %llx %llx", &f[0], &f[1], &f[2], &f[3]) == 4 && f[3]) {
                    info.skip = (long long)f[1];
                    info.valid = (long long)f[3];
                    return true;
                }
            }
        }
    }

    // Sound track edit list: media_time = priming, segment_duration = valid length.
    uint32_t movieScale = 0;
    bool found = false;
    struct Walker {
        std::vector<unsigned char>& b;
        uint32_t& movieScale;
        GaplessInfo& info;
        bool& found;
        void Walk(size_t start, size_t end, bool* isSound, uint32_t* mediaScale, long long* mediaTime, uint64_t* segment) {
            size_t p = start;
            while (p + 8 <= end) {
                uint64_t size = BE32(&b[p]);
                size_t hl = 8;
                if (size == 1 && p + 16 <= end) { size = BE64(&b[p + 8]); hl = 16; }
                if (size < hl || p + size > end) break;
                const unsigned char* type = &b[p + 4];
                size_t c = p + hl, ce = p + (size_t)size;
                if (!memcmp(type, "mvhd", 4) && ce - c >= 24) {
                    movieScale = BE32(&b[c + (b[c] == 1 ? 20 : 12)]);
                } else if (!memcmp(type, "trak", 4)) {
                    bool sound = false;
                    uint32_t scale = 0;
                    long long mt = -1;
                    uint64_t seg = 0;
                    Walk(c, ce, &sound, &scale, &mt, &seg);
                    if (sound && mt >= 0 && !found) {
                        info.skip = mt;
                        info.valid = movieScale && scale ? (long long)(seg * scale / movieScale) : 0;
                        found = true;
                    }
                } else if (!memcmp(type, "mdia", 4) || !memcmp(type, "edts", 4)) {
                    Walk(c, ce, isSound, mediaScale, mediaTime, segment);
                } else if (!memcmp(type, "hdlr", 4) && ce - c >= 12 && isSound) {
                    *isSound = !memcmp(&b[c + 8], "soun", 4);
                } else if (!memcmp(type, "mdhd", 4) && ce - c >= 24 && mediaScale) {
                    *mediaScale = BE32(&b[c + (b[c] == 1 ? 20 : 12)]);
                } else if (!memcmp(type, "elst", 4) && ce - c >= 8 && mediaTime) {
                    bool v1 = b[c] == 1;
                    uint32_t count = BE32(&b[c + 4]);
                    size_t e = c + 8;
                    for (uint32_t k = 0; k < count && e + (v1 ? 20 : 12) <= ce; ++k) {
                        uint64_t dur = v1 ? BE64(&b[e]) : BE32(&b[e]);
                        long long t = v1 ? (long long)BE64(&b[e + 8]) : (int32_t)BE32(&b[e + 4]);
                        e += v1 ? 20 : 12;
                        if (t < 0) continue;  // empty edit
                        *mediaTime = t;
                        *segment = dur;
                        break;
                    }
                }
                p += (size_t)size;
            }
        }
    } walker{moov, movieScale, info, found};
    walker.Walk(0, moov.size(), nullptr, nullptr, nullptr, nullptr);
    return found && info.skip > 0;
}

