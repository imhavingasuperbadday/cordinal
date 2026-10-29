// Reproducible, isolated memory workloads; never opens the user's library or media.
// Run as cordinal-test.exe --memory-benchmark idle|paused|library|scheduler|scheduler-reference|mp3|flac|ogg|m4a <fixtures> <log> [count].
// "library" builds a large synthetic library and cycles it through save/load; the format modes edit
// a copied sine fixture with a 128 MiB appended payload. Each stage is logged as one Snapshot line,
// and the last line is result=PASS|FAIL (exit code 0/1).
#include "app.h"

#include <array>

namespace {
// One machine-readable stage line: process and library memory plus elapsed time since start. The
// field names/units are the benchmark's log format, so keep them stable.
void Snapshot(FILE* log, const wchar_t* stage, ULONGLONG start) {
    PROCESS_MEMORY_COUNTERS_EX m{};
    m.cb = sizeof(m);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m));
    fwprintf(log, L"%ls private_bytes=%zu peak_private_bytes=%zu working_set=%zu peak_working_set=%zu library_bytes=%zu elapsed_ms=%llu\n",
             stage, m.PrivateUsage, m.PeakPagefileUsage, m.WorkingSetSize, m.PeakWorkingSetSize, g_lib.MemoryBytes(), GetTickCount64() - start);
    fflush(log);
}

// Appends a fixed 128 MiB payload of the standard test pattern. For MP4 a `free` box header goes
// first so the bytes parse as a legitimate (skippable) box rather than trailing garbage.
bool AppendPayload(const std::wstring& path, bool mp4) {
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    constexpr DWORD bytes = 128 * 1024 * 1024;
    std::array<char, 64 * 1024> chunk{};
    for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = (char)(i * 31 + 7);
    DWORD written = 0;
    bool ok = true;
    if (mp4) {
        const char header[]{(char)(((bytes + 8) >> 24) & 255), (char)(((bytes + 8) >> 16) & 255),
                            (char)(((bytes + 8) >> 8) & 255), (char)((bytes + 8) & 255), 'f', 'r', 'e', 'e'};
        ok = WriteFile(h, header, sizeof(header), &written, nullptr) && written == sizeof(header);
    }
    for (DWORD n = 0; ok && n < bytes; n += (DWORD)chunk.size())
        ok = WriteFile(h, chunk.data(), (DWORD)chunk.size(), &written, nullptr) && written == chunk.size();
    CloseHandle(h);
    return ok;
}

// Re-reads the trailing 128 MiB and checks it still matches the pattern, proving the tag edit did
// not damage or truncate the audio payload.
bool VerifyPayload(const std::wstring& path) {
    FileSource file(path);
    constexpr uint64_t bytes = 128 * 1024 * 1024;
    if (!file.Valid() || file.Size() < bytes) return false;
    std::array<char, 64 * 1024> expected{}, actual{};
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = (char)(i * 31 + 7);
    for (uint64_t pos = file.Size() - bytes; pos < file.Size(); pos += actual.size())
        if (!file.Read(pos, actual.size(), actual.data()) || actual != expected) return false;
    return true;
}
}  // namespace

int RunMemoryBenchmark(const std::vector<std::wstring>& args) {
    // --memory-benchmark <workload> <fixtures> <log> [track count]
    if (args.size() < 4) return 2;
    FILE* log = nullptr;
    if (_wfopen_s(&log, args[3].c_str(), L"w, ccs=UTF-8") || !log) return 3;
    const ULONGLONG start = GetTickCount64();
    Snapshot(log, L"start", start);
    bool ok = true;
    std::wstring scratch = DirectoryOf(args[3]) + L"\\memory-benchmark-" + std::to_wstring(GetCurrentProcessId());
    // library mode: synthesize `count` tagged tracks, then measure save and load.
    if (args[1] == L"idle" || args[1] == L"paused") {
        Engine engine;
        std::wstring error;
        ok = engine.Init(nullptr, error);
        if (ok && args[1] == L"paused") {
            PlayItem item;
            item.path = args[2] + L"\\media\\sine.flac";
            item.token = 1;
            engine.Play(item);
            Sleep(250);
            engine.SetPaused(true);
            Sleep(3000);  // the fade, queued output and endpoint release have settled
            ok = engine.Paused() && engine.Position() > 0;
        }
        Sleep(500);  // device/worker initialization has settled
        Snapshot(log, L"idle", start);
        FILETIME created, exited, kernel1, user1, kernel2, user2;
        ULONG64 cycles1 = 0, cycles2 = 0;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel1, &user1);
        QueryProcessCycleTime(GetCurrentProcess(), &cycles1);
        Sleep(3000);
        QueryProcessCycleTime(GetCurrentProcess(), &cycles2);
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel2, &user2);
        auto ticks = [](FILETIME time) { return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
        fwprintf(log, L"idle_cpu_ms=%.3f\n", (ticks(kernel2) + ticks(user2) - ticks(kernel1) - ticks(user1)) / 10000.0);
        fwprintf(log, L"idle_cpu_cycles=%llu\n", (unsigned long long)(cycles2 - cycles1));
        engine.Shutdown();
    } else if (args[1] == L"library" || args[1] == L"scheduler" || args[1] == L"scheduler-reference") {
        unsigned count = args.size() > 4 ? (unsigned)wcstoul(args[4].c_str(), nullptr, 10) : 100000;
        if (count > 2000000) ok = false;
        for (unsigned i = 0; ok && i < count; ++i) {
            wchar_t path[160];
            swprintf_s(path, L"C:\\MemoryFixture\\Artist %05u\\Album %06u\\%02u - Track %06u.flac",
                       i / 100, i / 10, i % 10 + 1, i);
            TrackId id = g_lib.Add(path);
            TagData tags;
            tags.title = "Track " + std::to_string(i);
            tags.artist = "Artist " + std::to_string(i / 100);
            tags.albumArtist = tags.artist;
            tags.album = "Album " + std::to_string(i / 10);
            tags.genre = "Genre " + std::to_string(i % 8);
            tags.date = "2026";
            tags.track = i % 10 + 1;
            tags.duration = 180;
            tags.sampleRate = 44100;
            tags.channels = 2;
            tags.codec = Codec::FLAC;
            g_lib.ApplyTags(id, tags);
            g_lib[id].flags |= Track::kInLibrary;
        }
        Snapshot(log, L"populated", start);
        if (args[1] != L"library") {
            const bool reference = args[1] == L"scheduler-reference";
            LARGE_INTEGER frequency, begin, end;
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&begin);
            std::vector<TrackId> selected;
            for (int pass = 0; pass < 20; ++pass) {
                if (reference) {
                    // Previous scheduler: materialize and sort every unmeasured track.
                    std::vector<std::pair<AlbumId, TrackId>> rest;
                    for (TrackId id = 0; id < g_lib.Count(); ++id) rest.emplace_back(AlbumKeyOf(g_lib[id]), id);
                    std::sort(rest.begin(), rest.end());
                    selected.clear();
                    for (size_t i = 0; i < std::min(size_t(300), rest.size()); ++i) selected.push_back(rest[i].second);
                    if (!pass) Snapshot(log, L"selecting", start);
                } else {
                    selected = app::AutoMeasureTracks({}, 300);
                    if (!pass) Snapshot(log, L"selecting", start);
                }
            }
            QueryPerformanceCounter(&end);
            ok = ok && selected.size() == std::min(size_t(300), g_lib.Count());
            fwprintf(log, L"selection_ms=%.3f selected=%zu\n", 1000.0 * (end.QuadPart - begin.QuadPart) / frequency.QuadPart / 20, selected.size());
        } else {
            std::vector<Playlist> playlists{{L"Library", {}, PlaylistKind::Library}, {L"All tracks", {}}};
            playlists[1].items.reserve(g_lib.Count());
            for (TrackId id = 0; id < g_lib.Count(); ++id) playlists[1].items.push_back(id);
            const std::wstring db = scratch + L".cdb";
            ok = ok && SaveLibrary(db, playlists);
            Snapshot(log, L"saved", start);
            ok = ok && LoadLibrary(db, playlists) && g_lib.Count() == count;
            Snapshot(log, L"loaded", start);
            DeleteFileW(db.c_str());
            DeleteFileW((db + L".bak").c_str());
        }
    } else if (args[1] == L"mp3" || args[1] == L"flac" || args[1] == L"ogg" || args[1] == L"m4a") {
        const std::wstring path = scratch + L"." + args[1];
        ok = CopyFileW((args[2] + L"\\media\\sine." + args[1]).c_str(), path.c_str(), TRUE) &&
             AppendPayload(path, args[1] == L"m4a");
        Snapshot(log, L"fixture_ready", start);
        TagEdit edit;
        edit.title = "Memory benchmark";
        edit.lyrics = std::string(8192, 'l');
        std::wstring error;
        ok = ok && WriteTags(path, edit, error);
        Snapshot(log, L"edited", start);
        TagData tags;
        std::string lyrics;
        ok = ok && ReadTags(path, tags) && tags.title == *edit.title && ReadLyrics(path, lyrics) && lyrics == *edit.lyrics;
        ok = ok && VerifyPayload(path);
        fwprintf(log, L"error=%ls\n", error.c_str());
        DeleteFileW(path.c_str());
    } else {
        ok = false;
    }
    fwprintf(log, L"result=%ls\n", ok ? L"PASS" : L"FAIL");
    fclose(log);
    return ok ? 0 : 1;
}
