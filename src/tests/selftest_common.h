// Shared declarations for the split selftest suite (see selftest.cpp): the includes every test
// file needs, the check helper and the scratch-file/decode/message-pump helpers several of them
// use (defined once in selftest.cpp), and the test functions RunSelfTest calls. The tests
// themselves live in the selftest_*.cpp files, one area per file.
#pragma once

#include "app.h"
#include "crashdump.h"
#include "audio_output.h"
#include "autoplaylist.h"
#include "cue.h"
#include "decoder.h"
#include "dsp.h"
#include "engine.h"
#include "reference_audio.h"
#include "game_music.h"
#include "inflate.h"
#include "library.h"
#include "listenbrainz.h"
#include "organize.h"
#include "peaks.h"
#include "playlog.h"
#include "points.h"
#include "scanner.h"
#include "sfx.h"
#include "smtc.h"
#include "steam.h"
#include "tagundo.h"
#include "tagwriter.h"
#include "tags.h"
#include "testlog.h"
#include "titleformat.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <crtdbg.h>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// Log file of the current --selftest run, for the checks that report extra detail.
extern FILE* g_log;

// The suite's check helper, defined here once (testlog.h collects it in the running group).
void Check(testlog::Condition ok, const wchar_t* fmt, ...);
// A step that prepares a fixture, a server or a key rather than asserting anything about cordinal:
// reported as a failure when it fails, and not counted as a check when it works.
bool Ready(testlog::Condition ok, const wchar_t* what);
bool HeadlessTests();
uint32_t TestSeed();
void TestHarness(const std::wstring& scratch);
void TestSortingProperties();
void TestPlaylistFiles(const std::wstring& scratch);
void TestSearchProperties();
void TestLibraryWorkflow(const std::wstring& scratch);
void TestImportedFolders(const std::wstring& scratch);
void TestMigration(const std::wstring& scratch);

// Scratch files: write/read a whole file, and decode a whole file to interleaved float.
bool WriteTestBytes(const std::wstring& file, std::string_view bytes);
std::string ReadTestBytes(const std::wstring& file);
std::vector<float> DecodeAll(const std::wstring& path, unsigned& rate);

// Pumps the message queue for `ms`, or until `done` returns true, so worker threads and posted
// messages (scanner, engine, peaks) make progress while a test waits.
void Pump(DWORD ms, const std::function<bool()>& done = {});

// Library checks (selftest_library.cpp, selftest_library_files.cpp).
void TestTitleFormat();
void TestExplorerMenu();
void TestWeb(const std::wstring& scratch);
void TestWebPreferences(const std::wstring& scratch);
void TestSearch();
bool ReferenceSearch(const Library& library, TrackId id, const std::vector<std::wstring>& words);
void TestAudioHashSection(const std::wstring& scratch);
void TestLibraryDb(const std::wstring& scratch);
void TestPlayLog(const std::wstring& scratch);
void TestListeningModel();
void TestEqualizer();
void TestVisuals();
void TestHeadsets();
void TestPoints();
void TestAutoPlaylists();
void TestOrganize(const std::wstring& media, const std::wstring& scratch);
void TestDeviceCopy(const std::wstring& media, const std::wstring& scratch);
void TestScanner(const std::wstring& media, const std::wstring& scratch);
void TestTagUndo(const std::wstring& media, const std::wstring& scratch);
void TestAudioHash(const std::wstring& media, const std::wstring& scratch);
void TestScanMerge();

// Audio checks (selftest_audio.cpp, selftest_engine.cpp, selftest_gamemusic.cpp).
void TestGapless(const std::wstring& dir);
void TestRanges(const std::wstring& root, const std::wstring& scratch);
void TestResampler();
void TestSoundEffects();
void TestLoops(const std::wstring& scratch);
void TestCue(const std::wstring& media, const std::wstring& scratch);
void TestSampleConversion();
void TestLiveAudio(const std::wstring& media);
void TestRadioDirectory(const std::wstring& scratch);
void TestRadioStream();
void TestHls();
void TestExclusive(const std::wstring& root, HWND hwnd);
void TestFadeRamp();
void TestEngine(const std::wstring& gapDir, const std::wstring& cueDir, const std::wstring& media);
void TestInflate();
void TestGameMusic(const std::wstring& dir);

// Exact PCM reading and its MD5 (selftest_exact.cpp), and cordinal's own format (selftest_cord.cpp).
void TestMd5();
void TestExactPcm(const std::wstring& root, const std::wstring& scratch);
void TestCordFormat(const std::wstring& root, const std::wstring& scratch);
void TestCordConversion(const std::wstring& root, const std::wstring& scratch);

// Tag checks (selftest_tags.cpp) and DSP checks (selftest_dsp.cpp).
void TestTags(const std::wstring& dir);
void TestExtendedTags(const std::wstring& dir);
void TestReplayGainTags(const std::wstring& root, const std::wstring& scratchDir);
void TestSteam(const std::wstring& scratch);
void TestChapters(const std::wstring& scratch);
void TestLyrics();
void TestCrashDump(const std::wstring& scratch);
void TestSmtc();
void TestListenBrainz(const std::wstring& scratch);
void TestLoudness();
void TestTruePeak();
void TestPeakLimiter();
void TestEffects();
void TestReplayGainModel();
std::string MakeSineWav(double seconds, double silentFrom);
void TestPeaks(const std::wstring& scratch);

// UI checks (selftest_ui.cpp) and the tests that live in other translation units.
void TestUiControls(const std::wstring& scratch);
// Packs (selftest_packs.cpp): the readers headless, and turning a pack on and off in the window.
void TestPacks(const std::wstring& scratch);
void TestPackIntegration(const std::wstring& scratch);
// Helpers (selftest_helpers.cpp), with the test companion as the helper program it runs: headless,
// and in the window.
void TestHelpers(const std::wstring& scratch);
void TestHelperIntegration(const std::wstring& scratch);
void TestPortedStorage(const std::wstring& directory);
void TestPortedUi(const std::wstring& directory);
void TestNativeCodecs(const std::wstring& root, const std::wstring& scratch);
void TestNesExpansion();
// ZIP archives, DSD, tracker modules, SID and MIDI (selftest_formats.cpp).
void TestFormats(const std::wstring& scratch);
void TestYm2612();
int TestMemoryBehavior(const std::wstring& scratchDir, FILE* log);
int TestSizeRegressions(const std::wstring& scratchDir, FILE* log);
