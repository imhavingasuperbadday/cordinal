// Named, independently selectable feature suites. Headless runs keep production parsers, workers
// and offline audio, while explicitly skipping native controls and physical output devices.
#include "selftest_common.h"
#include "test_report.h"
#include <cerrno>
#include <exception>

namespace {
bool headless = false;
uint32_t seed = 1;
enum : unsigned { Media = 1, Gap = 2, Rg = 4, Tags = 8, Native = 16, Window = 1, Device = 2 };
struct Suite {
    const wchar_t *id, *name, *source;
    unsigned fixtures, needs;
    std::function<void(const std::wstring&)> run;
};

std::vector<Suite> Suites(const std::wstring& root) {
    return {
        {L"harness", L"assertions and report contracts", L"src/tests/selftest_harness.cpp", 0, 0,
            [](const auto& s) { TestHarness(s); }},
        {L"size", L"size regressions", L"src/tests/size_regressions.cpp", 0, 0,
            [](const auto& s) { TestSizeRegressions(s, g_log); }},
        {L"title-format", L"title formatting", L"src/tests/selftest_library.cpp", 0, 0,
            [](const auto&) { TestTitleFormat(); }},
        {L"search", L"library search", L"src/tests/selftest_library.cpp", 0, 0,
            [](const auto&) { TestSearch(); TestSearchProperties(); }},
        {L"sorting", L"stable sorting and ownership", L"src/tests/selftest_workflows.cpp", 0, 0,
            [](const auto&) { TestSortingProperties(); }},
        {L"playlist-files", L"playlist import/export and failed writes", L"src/tests/selftest_workflows.cpp", 0, 0,
            [](const auto& s) { TestPlaylistFiles(s); }},
        {L"library-workflow", L"library edits across save and reopen", L"src/tests/selftest_workflows.cpp", 0, 0,
            [](const auto& s) { TestLibraryWorkflow(s); TestImportedFolders(s); }},
        {L"web", L"browser library, HTTP and HTTPS, streaming, pairing and away access", L"src/tests/selftest_web.cpp", 0, 0,
            [](const auto& s) { TestWeb(s); }},
        {L"storage", L"storage and playlist protections", L"src/tests/port_regressions.cpp", 0, 0,
            [](const auto& s) { TestPortedStorage(s); }},
        {L"library-db", L"library.cdb and audio hash sections", L"src/tests/selftest_library.cpp", 0, 0,
            [](const auto& s) { TestLibraryDb(s + L"\\library"); TestAudioHashSection(s + L"\\hash"); }},
        {L"play-log", L"play log and listening statistics", L"src/tests/selftest_library.cpp", 0, 0,
            [](const auto& s) { TestPlayLog(s + L"\\plays"); }},
        {L"migration", L"backups, path remapping and playback statistics import", L"src/tests/selftest_migration.cpp", 0, 0,
            [](const auto& s) { TestMigration(s); }},
        {L"listening-model", L"listening model", L"src/tests/selftest_library.cpp", 0, 0,
            [](const auto&) { TestListeningModel(); }},
        {L"headsets", L"headphone recognition and protocol", L"src/tests/selftest_headsets.cpp", 0, 0,
            [](const auto&) { TestHeadsets(); }},
        {L"explorer", L"Explorer folder menu and its command line", L"src/tests/selftest_explorer.cpp", 0, 0,
            [](const auto&) { TestExplorerMenu(); }},
        {L"points", L"cordinal points", L"src/tests/selftest_library.cpp", 0, 0,
            [](const auto&) { TestPoints(); }},
        {L"memory", L"memory and worker lifetimes", L"src/tests/memorytest.cpp", 0, 0,
            [](const auto& s) { TestMemoryBehavior(s, g_log); }},
        {L"packs", L"packs: readers, $define functions and theme contrast", L"src/tests/selftest_packs.cpp", 0, 0,
            [](const auto& s) { TestPacks(s); }},
        {L"helpers", L"helpers: manifests, child processes, decoding, tags, trust and the visual feed",
            L"src/tests/selftest_helpers.cpp", 0, 0, [](const auto& s) { TestHelpers(s); }},
        {L"auto-playlists", L"auto-playlist rules and limits", L"src/tests/selftest_library.cpp", 0, 0,
            [](const auto&) { TestAutoPlaylists(); }},
        {L"tags", L"tag reading and extended metadata", L"src/tests/selftest_tags.cpp", Media | Tags, 0,
            [root](const auto&) { TestTags(root + L"\\media"); TestExtendedTags(root + L"\\tags2"); }},
        {L"native-codecs", L"native codecs without fallback", L"src/tests/native_codecs.cpp", Media | Native, 0,
            [root](const auto& s) { TestNativeCodecs(root, s); }},
        {L"live-audio", L"sequential radio audio decoding", L"src/tests/selftest_live_audio.cpp", Media, 0,
            [root](const auto&) { TestLiveAudio(root + L"\\media"); }},
        {L"radio", L"radio directory and favorites", L"src/tests/selftest_radio.cpp", 0, 0,
            [](const auto& s) { TestRadioDirectory(s); TestRadioStream(); TestHls(); }},
        {L"exact-pcm", L"exact PCM and MD5", L"src/tests/selftest_exact.cpp", Media | Gap | Rg | Tags | Native, 0,
            [root](const auto& s) { TestMd5(); TestExactPcm(root, s); }},
        {L"cord", L"cordinal format", L"src/tests/selftest_cord.cpp", Media, 0,
            [root](const auto& s) { TestCordFormat(root, s); }},
        {L"cord-conversion", L".cord conversion and audio export", L"src/tests/selftest_cord.cpp", Media | Tags | Rg, 0,
            [root](const auto& s) { TestCordConversion(root, s); }},
        {L"gapless", L"gapless decoder joins", L"src/tests/selftest_audio.cpp", Gap, 0,
            [root](const auto&) { TestGapless(root + L"\\gap2"); }},
        {L"ranges", L"source ranges", L"src/tests/selftest_audio.cpp", Gap, 0,
            [root](const auto& s) { TestRanges(root, s); }},
        {L"loops", L"game-music loops and sections", L"src/tests/selftest_audio.cpp", 0, 0,
            [](const auto& s) { TestLoops(s); }},
        {L"resampler", L"resampler", L"src/tests/selftest_audio.cpp", 0, 0,
            [](const auto&) { TestResampler(); }},
        {L"equalizer", L"parametric EQ and crossfeed", L"src/tests/selftest_dsp.cpp", 0, 0,
            [](const auto&) { TestEqualizer(); TestVisuals(); }},
        {L"effects", L"reverb, tempo and smart volume", L"src/tests/selftest_dsp.cpp", 0, 0,
            [](const auto&) { TestEffects(); }},
        {L"sound-effects", L"sound effects and exact game samples", L"src/tests/selftest_audio.cpp", 0, 0,
            [](const auto&) { TestSoundEffects(); }},
        {L"loudness", L"ReplayGain, loudness and limiter", L"src/tests/selftest_dsp.cpp", 0, 0,
            [](const auto&) { TestLoudness(); TestTruePeak(); TestPeakLimiter(); TestReplayGainModel(); }},
        {L"replaygain-tags", L"ReplayGain tag round trips", L"src/tests/selftest_tags.cpp", Rg | Gap, 0,
            [root](const auto& s) { TestReplayGainTags(root, s); }},
        {L"peaks", L"waveform peaks and cancellation", L"src/tests/selftest_dsp.cpp", 0, 0,
            [](const auto& s) { TestPeaks(s); }},
        {L"scanner", L"folder scanning and watching", L"src/tests/selftest_library_files.cpp", Media, 0,
            [root](const auto& s) { TestScanner(root + L"\\media", s + L"\\scan"); }},
        {L"organize", L"organize files and rollback", L"src/tests/selftest_library_files.cpp", Media, 0,
            [root](const auto& s) { TestOrganize(root + L"\\media", s + L"\\organize"); }},
        {L"copy", L"copy to folder or device", L"src/tests/selftest_copy.cpp", Media | Tags, 0,
            [root](const auto& s) { TestDeviceCopy(root + L"\\media", s + L"\\copy"); }},
        {L"tag-undo", L"tag undo", L"src/tests/selftest_library_files.cpp", Media, 0,
            [root](const auto& s) { TestTagUndo(root + L"\\media", s); }},
        {L"duplicates", L"duplicate audio", L"src/tests/selftest_library_files.cpp", Media, 0,
            [root](const auto& s) { TestAudioHash(root + L"\\media", s); }},
        {L"cue", L"CUE sheets", L"src/tests/selftest_audio.cpp", Media, 0,
            [root](const auto& s) { TestCue(root + L"\\media", s); }},
        {L"gzip", L"gzip parsing", L"src/tests/selftest_gamemusic.cpp", 0, 0,
            [](const auto&) { TestInflate(); }},
        {L"game-music", L"game music", L"src/tests/selftest_gamemusic.cpp", 0, 0,
            [](const auto& s) { TestGameMusic(s); }},
        {L"formats", L"ZIP archives, DSD, tracker modules, SID and MIDI", L"src/tests/selftest_formats.cpp", 0, 0,
            [](const auto& s) { TestFormats(s); }},
        {L"nes", L"NES expansion audio", L"src/tests/nes_expansion_test.cpp", 0, 0,
            [](const auto&) { TestNesExpansion(); }},
        {L"ym2612", L"YM2612 audio", L"src/tests/ym2612_test.cpp", 0, 0,
            [](const auto&) { TestYm2612(); }},
        {L"tag-writing-mp4", L"tag writing: MP3 and MP4", L"src/tests/tagwriter_test.cpp", Media | Tags, 0,
            [root](const auto& s) { TestTagWriterMp3Mp4(root, s, g_log); }},
        {L"tag-writing-ogg", L"tag writing: FLAC, Ogg and Opus", L"src/tests/tagwriter_test.cpp", Media | Tags | Rg, 0,
            [root](const auto& s) { TestTagWriterFlacOgg(root, s, g_log); }},
        {L"steam", L"Steam soundtracks", L"src/tests/selftest_tags.cpp", 0, 0,
            [](const auto& s) { TestSteam(s); }},
        {L"chapters-lyrics", L"chapters and lyrics", L"src/tests/selftest_tags.cpp", 0, 0,
            [](const auto& s) { TestChapters(s); TestLyrics(); }},
        {L"crash-dumps", L"crash dumps", L"src/tests/selftest_tags.cpp", 0, 0,
            [](const auto& s) { TestCrashDump(s); }},
        {L"media-controls", L"Windows media controls (SMTC)", L"src/tests/selftest_tags.cpp", 0, Window,
            [](const auto&) { TestSmtc(); }},
        {L"listenbrainz", L"ListenBrainz queue and privacy", L"src/tests/selftest_tags.cpp", 0, 0,
            [](const auto& s) { TestListenBrainz(s); }},
        {L"sample-conversion", L"sample conversion and click-free fades", L"src/tests/selftest_engine.cpp", 0, 0,
            [](const auto&) { TestSampleConversion(); TestFadeRamp(); }},
        {L"engine", L"live playback, seek, pause and gapless timing", L"src/tests/selftest_engine.cpp", Gap | Media, Device,
            [root](const auto& s) {
                const std::wstring album = s + L"\\cue";
                CreateDirectoryW(album.c_str(), nullptr);
                Ready(WriteTestBytes(album + L"\\Ramp.wav", MakeSineWav(2, 2)) &&
                    WriteTestBytes(album + L"\\Ramp.cue", "FILE \"Ramp.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
                        "TRACK 02 AUDIO\nINDEX 01 00:00:70\nTRACK 03 AUDIO\nINDEX 01 00:01:20\n"),
                    L"engine CUE fixture is prepared independently of the CUE suite");
                TestEngine(root + L"\\gap2", album, root + L"\\media");
            }},
        {L"ui", L"native UI, commands, preferences, themes and workers", L"src/tests/selftest_ui.cpp", 0, Window,
            [](const auto& s) { TestUiControls(s); }},
        {L"scan-merge", L"scan merging and offline roots", L"src/tests/selftest_library_files.cpp", 0, 0,
            [](const auto&) { TestScanMerge(); }}
    };
}

std::wstring Requirements(const Suite& s) {
    std::wstring out = s.needs & Window ? L"window" : s.needs & Device ? L"audio-device" : L"headless";
    const wchar_t* folders[] = {L"media", L"gap2", L"rg", L"tags2", L"native"};
    for (unsigned i = 0; i < std::size(folders); ++i)
        if (s.fixtures & (1u << i)) out += L"," + std::wstring(folders[i]);
    return out;
}

bool FixturesExist(unsigned mask, const std::wstring& root) {
    const std::vector<std::vector<std::wstring>> files = {
        {L"media\\sine.flac", L"media\\sine.mp3", L"media\\sine.m4a", L"media\\alac.m4a", L"media\\sine.ogg",
         L"media\\sine.opus", L"media\\sine.wav", L"media\\sine.wma"},
        {L"gap2\\a.flac", L"gap2\\b.flac", L"gap2\\a.mp3", L"gap2\\b.mp3", L"gap2\\a.m4a", L"gap2\\b.m4a",
         L"gap2\\a.ogg", L"gap2\\b.ogg", L"gap2\\ref.wav"},
        {L"rg\\rg.flac", L"rg\\rg.mp3", L"rg\\rg.ogg", L"rg\\rg.opus"},
        {L"tags2\\full.flac", L"tags2\\full.mp3", L"tags2\\full.m4a", L"tags2\\full.ogg"},
        {L"native\\sine.wv", L"native\\sine.ac3", L"native\\sine.mka"}
    };
    bool ok = true;
    for (unsigned i = 0; i < files.size(); ++i) if (mask & (1u << i)) for (const auto& name : files[i]) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        bool exists = GetFileAttributesExW((root + L"\\" + name).c_str(), GetFileExInfoStandard, &data) &&
            !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (data.nFileSizeHigh || data.nFileSizeLow);
        if (!exists) {
            Check(false, L"required fixture missing or empty: %ls; run tools\\make-fixtures.ps1 -Out \"%ls\"",
                name.c_str(), root.c_str());
            ok = false;
        }
    }
    return ok;
}

std::vector<std::wstring> SplitSuites(const std::wstring& text) {
    std::vector<std::wstring> result;
    size_t at = 0;
    do {
        size_t end = text.find(L',', at);
        result.push_back(text.substr(at, end == text.npos ? text.npos : end - at));
        if (end == text.npos) break;
        at = end + 1;
    } while (true);
    return result;
}
}

bool HeadlessTests() { return headless; }
uint32_t TestSeed() { return seed; }

int ListSelfTests() {
    fwprintf(stdout, L"suite\trequirements\tfeature (source)\n");
    for (const auto& s : Suites(L""))
        fwprintf(stdout, L"%ls\t%ls\t%ls (%ls)\n", s.id, Requirements(s).c_str(), s.name, s.source);
    return 0;
}

int RunSelfTest(const std::vector<std::wstring>& args) {
    if (args.size() < 3) { fwprintf(stderr, L"Usage: --selftest <fixtures> <log> [--headless] [--suite id,...] [--seed N] [--fail-fast]\n"); return 2; }
    headless = false; seed = 1;
    bool failFast = false;
    std::vector<std::wstring> patterns;
    for (size_t i = 3; i < args.size(); ++i) {
        if (args[i] == L"--headless") headless = true;
        else if (args[i] == L"--fail-fast") failFast = true;
        else if (args[i] == L"--suite" && i + 1 < args.size()) {
            auto next = SplitSuites(args[++i]); patterns.insert(patterns.end(), next.begin(), next.end());
        } else if (args[i] == L"--seed" && i + 1 < args.size()) {
            const auto& number = args[++i];
            wchar_t* end = nullptr; errno = 0;
            unsigned long long value = wcstoull(number.c_str(), &end, 10);
            if (number.empty() || number[0] == L'-' || *end || errno || value > UINT32_MAX) {
                fwprintf(stderr, L"Invalid seed: %ls\n", number.c_str()); return 2;
            }
            seed = (uint32_t)value;
        } else { fwprintf(stderr, L"Unknown or incomplete self-test option: %ls\n", args[i].c_str()); return 2; }
    }
    const std::wstring root = NormalizeMediaPath(args[1]), log = NormalizeMediaPath(args[2]);
    if (root.empty() || log.empty()) { fwprintf(stderr, L"Invalid fixtures or log path\n"); return 2; }
    auto suites = Suites(root);
    for (const auto& pattern : patterns) {
        if (pattern.empty() || std::none_of(suites.begin(), suites.end(), [&](const auto& s) {
                return PathMatchSpecW(s.id, pattern.c_str()) != FALSE;
            })) { fwprintf(stderr, L"No suite matches '%ls'; use --list-tests\n", pattern.c_str()); return 2; }
    }
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::error_code ec;
    std::filesystem::create_directories(DirectoryOf(log), ec);
    if (ec || _wfopen_s(&g_log, log.c_str(), L"w, ccs=UTF-8") || !g_log) {
        fwprintf(stderr, L"Cannot open self-test log: %ls\n", log.c_str()); return 3;
    }
    auto& state = testlog::state();
    state = {}; state.log = g_log; state.console = true;
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    TestReport report; report.log = log; report.fixtures = root; report.headless = headless; report.seed = seed;
    fwprintf(g_log, L"cordinal self-test headless=%d seed=%u fixtures=%ls\n", headless, seed, root.c_str());
    bool reportsOk = true;
    for (const auto& s : suites) {
        if (!patterns.empty() && std::none_of(patterns.begin(), patterns.end(), [&](const auto& pattern) {
                return PathMatchSpecW(s.id, pattern.c_str()) != FALSE;
            })) continue;
        SuiteResult result; result.id = s.id; result.name = s.name; result.source = s.source;
        result.requirements = Requirements(s);
        result.artifacts = log + L".files\\" + s.id;
        report.suites.push_back(std::move(result));
        auto& current = report.suites.back();
        fwprintf(g_log, L"START %ls source=%ls artifacts=%ls\n", s.id, s.source, current.artifacts.c_str());
        fflush(g_log);
        fwprintf(stdout, L"START %ls\n", s.id); fflush(stdout);
        reportsOk = WriteTestReports(report) && reportsOk;
        const int checks = state.checks, failures = state.failures, skipped = state.skipped;
        const size_t details = state.details.size(), skips = state.skips.size();
        const ULONGLONG started = GetTickCount64();
        if (headless && s.needs) {
            current.status = L"skipped";
            current.reason = s.needs & Window ? L"requires native Windows UI; rerun without --headless" :
                L"requires a physical audio device; rerun without --headless";
            fwprintf(g_log, L"SKIP %ls: %ls\n", s.id, current.reason.c_str());
            fwprintf(stdout, L"SKIP %ls: %ls\n", s.id, current.reason.c_str());
        } else {
            testlog::Group group(s.name);
            ec.clear(); std::filesystem::create_directories(current.artifacts, ec);
            if (ec) Check(false, L"suite scratch directory cannot be created: %ls", current.artifacts.c_str());
            if (!ec && FixturesExist(s.fixtures, root)) {
                try { s.run(current.artifacts); }
                catch (const std::exception& e) { Check(false, L"uncaught exception: %hs", e.what()); }
                catch (...) { Check(false, L"uncaught non-standard exception"); }
            }
            if (state.checks == checks && state.skipped == skipped)
                Check(false, L"suite produced no feature assertions");
            current.status = state.failures == failures ? L"passed" : L"failed";
        }
        current.milliseconds = (double)(GetTickCount64() - started);
        current.checks = state.checks - checks; current.failures = state.failures - failures;
        current.skippedChecks = state.skipped - skipped;
        current.details.assign(state.details.begin() + details, state.details.end());
        current.skips.assign(state.skips.begin() + skips, state.skips.end());
        fwprintf(g_log, L"END %ls status=%ls checks=%d failures=%d skipped=%d ms=%.0f\n", s.id, current.status.c_str(),
            current.checks, current.failures, current.skippedChecks, current.milliseconds);
        fflush(g_log);
        fwprintf(stdout, L"%ls %ls (%d checks, %d failed, %.0f ms)\n", current.status == L"failed" ? L"FAIL" :
            current.status == L"skipped" ? L"SKIP" : L"PASS", s.id, current.checks, current.failures, current.milliseconds);
        fflush(stdout);
        reportsOk = WriteTestReports(report) && reportsOk;
        if (failFast && current.failures) break;
    }
    sfx::Shutdown();
    report.complete = true;
    reportsOk = WriteTestReports(report) && reportsOk;
    if (!reportsOk) { fwprintf(stderr, L"Cannot publish JSON/JUnit reports beside %ls\n", log.c_str()); }
    int skippedSuites = 0;
    for (const auto& s : report.suites) skippedSuites += s.status == L"skipped";
    fwprintf(g_log, L"checks=%d skipped=%d\nfailures=%d\n", state.checks, state.skipped + skippedSuites, state.failures);
    fwprintf(stdout, L"checks=%d failures=%d skipped=%d\nReports: %ls.json, %ls.xml\n", state.checks,
        state.failures, state.skipped + skippedSuites, log.c_str(), log.c_str());
    fclose(g_log); g_log = nullptr; testlog::SetLog(nullptr);
    if (SUCCEEDED(com)) CoUninitialize();
    return state.failures ? state.failures : reportsOk ? 0 : 3;
}
