// Helpers (helpers.h): manifests, command lines, child processes, the decoder and its failures,
// tags from an info command, a tool's tag proposal, trust records and the visual feed. The test
// companion is its own helper program: `cordinal-test.exe --fake-helper <mode>` prints what a real
// helper would, so no third-party program is needed. The suite copies the companion into a helper
// folder, as a user would copy a program there.
#include "selftest_common.h"

#include "helpers.h"

#include <thread>

namespace {

// Frame n, channel c of the fake helper's 16-bit signal: every value of the range comes up.
int16_t Signal16(uint64_t n, unsigned c) { return (int16_t)(int)((n * 37 + c * 12345) % 65536 - 32768); }
// And of its 24-bit mono signal.
int32_t Signal24(uint64_t n) { return (int32_t)((n * 4099) % 16777216) - 8388608; }

void WriteOut(HANDLE out, const std::string& bytes) {
    DWORD written = 0;
    WriteFile(out, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
}

void Put16(std::string& s, uint32_t v) { s += (char)(v & 0xFF), s += (char)(v >> 8 & 0xFF); }
void Put32(std::string& s, uint32_t v) { Put16(s, v & 0xFFFF), Put16(s, v >> 16); }

// A WAV header as a program writing to a pipe makes it: its sizes unknown (0xFFFFFFFF), unless
// `frames` gives them.
std::string WavHeader(unsigned rate, unsigned channels, unsigned bits, uint64_t frames, bool sized) {
    const uint32_t data = sized ? (uint32_t)(frames * channels * bits / 8) : 0xFFFFFFFF;
    std::string h = "RIFF";
    Put32(h, sized ? 4 + 24 + 14 + 8 + data : 0xFFFFFFFF);  // WAVE, fmt, LIST and data chunks
    h += "WAVEfmt ";
    Put32(h, 16), Put16(h, 1), Put16(h, channels), Put32(h, rate), Put32(h, rate * channels * bits / 8);
    Put16(h, channels * bits / 8), Put16(h, bits);
    h += "LIST";  // a chunk before the samples, as ffmpeg writes one
    Put32(h, 5);
    h += std::string("INFOx\0", 6);  // odd length: padded to 6
    h += "data";
    Put32(h, data);
    return h;
}

std::string Samples16(uint64_t first, uint64_t frames, unsigned channels) {
    std::string out;
    for (uint64_t n = first; n < first + frames; ++n)
        for (unsigned c = 0; c < channels; ++c) Put16(out, (uint16_t)Signal16(n, c));
    return out;
}

std::wstring Companion() {
    wchar_t self[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, self, (DWORD)std::size(self));
    return std::wstring(self, n);
}

std::string ReadAllOf(const Helper& h, const std::wstring& arguments, std::wstring& error, bool& complete, DWORD& code) {
    HelperProcess p;
    std::string out;
    complete = p.Start(h.dir + L"\\" + h.programs[0], arguments, h.dir, error) && p.ReadAll(out, 1 << 20);
    code = complete ? p.Finish(5000) : ERROR_GEN_FAILURE;
    return out;
}

std::vector<std::wstring> Lines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    for (size_t at = 0; at < text.size();) {
        size_t end = std::min(text.find(L'\n', at), text.size());
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return lines;
}

}  // namespace

// What the fake helper does, by its first argument after --fake-helper:
//   wav <seconds> <rate> <channels> <bits 16|24> <sized 0|1> <start> <file>   a WAV stream from <start>
//   crash <frames>      a 44.1 kHz stereo header and that many frames, then a complaint and exit code 3
//   hang | silent       the same header then nothing, or nothing at all, for a minute
//   linger              a header and 100 frames while a program it started holds stdout open
//   bad                 no WAV, a complaint and exit code 1
//   info | echo ...     tags as "key=value" lines | each argument on its own line
//   feed <handle>       what the visual feed holds, and whether it could be mapped for writing
//   sleep <ms>          prints "awake" after that long
int RunFakeHelper(const std::vector<std::wstring>& args) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    const std::wstring mode = args.size() > 1 ? args[1] : L"";
    auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::wstring(); };
    if (mode == L"wav") {
        const double seconds = _wtof(arg(2).c_str()), start = _wtof(arg(7).c_str());
        const unsigned rate = (unsigned)_wtoi(arg(3).c_str()), channels = (unsigned)_wtoi(arg(4).c_str()),
                       bits = (unsigned)_wtoi(arg(5).c_str());
        const uint64_t first = (uint64_t)std::llround(start * rate), total = (uint64_t)std::llround(seconds * rate);
        const uint64_t frames = total > first ? total - first : 0;
        WriteOut(out, WavHeader(rate, channels, bits, frames, arg(6) == L"1"));
        for (uint64_t at = 0; at < frames; at += 4096) {
            const uint64_t n = std::min<uint64_t>(4096, frames - at);
            std::string block;
            if (bits == 24)
                for (uint64_t k = first + at; k < first + at + n; ++k) {
                    const uint32_t v = (uint32_t)Signal24(k);
                    block += (char)(v & 0xFF), block += (char)(v >> 8 & 0xFF), block += (char)(v >> 16 & 0xFF);
                }
            else block = Samples16(first + at, n, channels);
            WriteOut(out, block);
        }
        return 0;
    }
    if (mode == L"crash" || mode == L"hang" || mode == L"linger") {
        if (mode == L"linger") {
            // A program the helper starts inherits its stdout and outlives it.
            STARTUPINFOW si{sizeof(si)};
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = out;
            si.hStdError = err;
            PROCESS_INFORMATION pi{};
            const std::wstring self = Companion();
            std::wstring command = L"\"" + self + L"\" --fake-helper sleep 3000";
            if (CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        const uint64_t frames = mode == L"crash" ? (uint64_t)_wtoi(arg(2).c_str()) : mode == L"linger" ? 100 : 0;
        WriteOut(out, WavHeader(44100, 2, 16, 0, false) + Samples16(0, frames, 2));
        if (mode == L"hang") Sleep(60000);
        if (mode == L"crash") {
            WriteOut(err, "decoding...\nit broke at frame 1000.\n");
            return 3;
        }
        return 0;
    }
    if (mode == L"silent" || mode == L"sleep") {
        Sleep(mode == L"silent" ? 60000 : (DWORD)_wtoi(arg(2).c_str()));
        WriteOut(out, "awake\n");
        return 0;
    }
    if (mode == L"bad") {
        WriteOut(out, "this is not a wav stream at all\n");
        WriteOut(err, "warming up\nunsupported codec\n");
        return 1;
    }
    if (mode == L"info") {
        WriteOut(out, "TAG:title=Fake Song\r\nartist=Someone\r\nduration=2.500000\r\nTAG:TRACK=3/9\r\nsample_rate=44100\r\n");
        return 0;
    }
    if (mode == L"echo") {
        std::wstring text;
        for (size_t i = 2; i < args.size(); ++i) text += args[i] + L"\n";
        WriteOut(out, ToUtf8(text));
        return 0;
    }
    if (mode == L"feed") {
        HANDLE mapping = (HANDLE)(uintptr_t)_wcstoui64(arg(2).c_str(), nullptr, 10);
        const auto* view = static_cast<const HelperFeed*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        void* writable = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0);
        auto copy = std::make_unique<HelperFeed>();
        if (!view || !ReadHelperFeed(view, *copy)) return 2;
        char text[160];
        sprintf_s(text, "%.4s %u %u %u %u %u %.3f %s\n", copy->magic, copy->version, copy->frames, copy->bands, copy->state, copy->rate,
                  copy->levels[0], writable ? "writable" : "read-only");
        WriteOut(out, text);
        return 0;
    }
    WriteOut(err, "unknown fake helper mode\n");
    return 2;
}

// In the window: a new helper that stays off, one the user turned on in an earlier run with its
// tool in the Edit menu, a tool run from there, a helper whose files changed, and safe mode.
void TestHelperIntegration(const std::wstring& scratch) {
    using namespace app;
    testlog::Group group(L"helpers in the window");
    if (ProcessElevated()) {
        testlog::Skip(L"helpers stay off while the self-test runs as administrator");
        return;
    }
    const std::wstring dataDir = g.dataDir, dir = scratch + L"\\helpers\\fake";
    std::error_code ec;
    std::filesystem::remove_all(scratch + L"\\helpers", ec);
    std::filesystem::create_directories(dir, ec);
    const std::string ini = "[helper]\nname = Fake\n[decoder]\nextensions = fake\n"
                            "decode = fake.exe --fake-helper wav 1 8000 2 16 0 {start} {file}\n"
                            "[tool]\nlabel = Say hello\ncommand = fake.exe --fake-helper echo hello\noutput = nothing\n";
    if (!Ready(!ec && CopyFileW(Companion().c_str(), (dir + L"\\fake.exe").c_str(), FALSE) && WriteTestBytes(dir + L"\\helper.ini", ini),
               L"a helper folder in the scratch profile"))
        return;
    g.dataDir = scratch;
    auto inMenu = [](int id, UINT state) {
        UINT found = GetMenuState(MainMenuBar(), id, MF_BYCOMMAND);
        return found != (UINT)-1 && (found & state) == state;
    };
    LoadHelpers(false);
    Check(!IsSupportedAudioFile(L"x.fake") && IniRead(L"Helpers", L"Known", L"") == L"fake" && IniRead(L"Helpers", L"On", L"").empty() &&
              inMenu(IDM_HELPER_BASE, 0) && !inMenu(IDM_HELPER_BASE, MF_CHECKED) && !inMenu(IDM_HELPER_TOOL_BASE, 0),
          L"a new helper is listed in View > Helpers and stays off until the user turns it on");
    // As if the user had turned it on in an earlier run; turning it on asks first, in a message box.
    IniWrite(L"Helpers", L"On", L"fake");
    IniWrite(L"Helpers", L"Trust.fake", MakeHelperTrust(dir));
    LoadHelpers(false);
    Check(IsSupportedAudioFile(L"x.fake") && inMenu(IDM_HELPER_BASE, MF_CHECKED) && inMenu(IDM_HELPER_TOOL_BASE, 0),
          L"a helper that is on plays its file types and puts its tool in Edit > Run");
    OnCommand(IDM_HELPER_TOOL_BASE, 0, nullptr);
    const bool started = !HelperToolRunning().empty();
    Pump(10000, [] { return HelperToolRunning().empty(); });
    Check(started && HelperToolRunning().empty() && g.message.find(L"Say hello finished.") != std::wstring::npos,
          L"a tool runs on a worker thread and says when it is done: %ls", g.message.c_str());
    WriteTestBytes(dir + L"\\helper.ini", ini + "; edited\n");
    OnCommand(IDM_HELPERS_RELOAD, 0, nullptr);
    Check(!IsSupportedAudioFile(L"x.fake") && IniRead(L"Helpers", L"On", L"").empty() &&
              g.message.find(L"Fake changed since you turned it on") != std::wstring::npos,
          L"a helper whose files changed is turned off, and the user is told");
    IniWrite(L"Helpers", L"On", L"fake");
    IniWrite(L"Helpers", L"Trust.fake", MakeHelperTrust(dir));
    LoadHelpers(false);
    OnCommand(IDM_HELPER_BASE, 0, nullptr);
    Check(!IsSupportedAudioFile(L"x.fake") && IniRead(L"Helpers", L"On", L"").empty() && IniRead(L"Helpers", L"Trust.fake", L"").empty(),
          L"turning a helper off forgets the record of its files");
    IniWrite(L"Helpers", L"On", L"fake");
    IniWrite(L"Helpers", L"Trust.fake", MakeHelperTrust(dir));
    LoadHelpers(true);
    HMENU menu = HelpersMenu();
    wchar_t first[128]{};
    GetMenuStringW(menu, 0, first, (int)std::size(first), MF_BYPOSITION);
    DestroyMenu(menu);
    Check(!IsSupportedAudioFile(L"x.fake") && std::wstring(first) == L"Helpers are off in safe mode", L"safe mode starts no helper");
    IniWrite(L"Helpers", L"On", L"");
    IniWrite(L"Helpers", L"Trust.fake", L"");
    IniWrite(L"Helpers", L"Known", L"");
    std::filesystem::remove_all(scratch + L"\\helpers", ec);
    g.dataDir = dataDir;
    LoadHelpers(false);
}

void TestHelpers(const std::wstring& scratch) {
    HRSRC manifest = FindResourceW(nullptr, MAKEINTRESOURCEW(1), RT_MANIFEST);
    HGLOBAL loaded = manifest ? LoadResource(nullptr, manifest) : nullptr;
    const char* bytes = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    DWORD size = manifest ? SizeofResource(nullptr, manifest) : 0;
    std::string xml = bytes ? std::string(bytes, size) : std::string();
    Check(xml.find("level=\"asInvoker\"") != std::string::npos,
          L"shared executable manifest runs at the invoking privilege level");
    const std::wstring dir = scratch + L"\\helpers\\fake", song = scratch + L"\\song.fake";
    std::error_code ec;
    std::filesystem::remove_all(scratch + L"\\helpers", ec);  // an earlier run's helper.ini would change what is read
    std::filesystem::create_directories(dir, ec);
    if (!Ready(!ec && CopyFileW(Companion().c_str(), (dir + L"\\fake.exe").c_str(), FALSE) && WriteTestBytes(song, "x"),
               L"a helper folder holds a copy of the test companion"))
        return;
    SetHelperPatience(15000);
    auto parse = [&](const std::string& ini) { return ParseHelper(L"fake", dir, ini); };
    const std::string head = "[helper]\nformat = 1\nname = Fake\nversion = 1.0\n";

    {
        testlog::Group group(L"helper.ini");
        const Helper h = parse(head +
                               "[decoder]\nextensions = fake, .FK2; *.flac cue\n"
                               "decode = fake.exe --fake-helper wav 2 44100 2 16 0 {start} {file}\n"
                               "info = fake.exe --fake-helper info {file}\n"
                               "[tool.echo]\nlabel = Echo\ncommand = \"fake.exe\" --fake-helper echo {file} \"{%title% - x}\" --flag={file} \"\"\n"
                               "[tool.outside]\nlabel = Outside\ncommand = ..\\outside.exe {file}\n"
                               "[tool.batch]\nlabel = Batch\ncommand = run.bat {file}\n"
                               "[tool.tags]\nlabel = Tags for all\ncommand = fake.exe {files}\noutput = tags\n"
                               "[tool.unknown]\nlabel = Unknown\ncommand = fake.exe {song}\n"
                               "[visualizer]\nlabel = Feed reader\ncommand = fake.exe --fake-helper feed {feed}\n"
                               "[visualizer.blind]\nlabel = Blind\ncommand = fake.exe --fake-helper feed\n");
        Check(h.error.empty() && h.name == L"Fake" && h.version == L"1.0", L"a helper.ini names its helper: %ls", h.error.c_str());
        Check(h.extensions == std::vector<std::wstring>{L"fake", L"fk2"}, L"file types are read without dots or stars, and lowercase");
        auto warned = [&](const wchar_t* part) {
            return std::any_of(h.warnings.begin(), h.warnings.end(), [&](auto& w) { return w.find(part) != std::wstring::npos; });
        };
        Check(warned(L".flac files are cordinal's own") && warned(L".cue files are cordinal's own"),
              L"a helper can't take a file type cordinal plays itself, or a CUE sheet");
        Check(h.tools.size() == 1 && h.tools[0].label == L"Echo" && h.tools[0].perTrack && !h.tools[0].together &&
                  warned(L"Outside is left out") && warned(L"Batch is left out") && warned(L"Tags for all is left out") &&
                  warned(L"{song}"),
              L"a tool runs a program in the helper's folder, never one outside it or a batch file, with placeholders cordinal fills in");
        Check(h.visualizers.size() == 1 && warned(L"Blind is left out"), L"a visualizer must take the feed");
        Check(h.programs == std::vector<std::wstring>{L"fake.exe"} && HelperProgramList(h) == L"fake.exe",
              L"every program a helper runs is listed once");
        Check(HelperSummary(h) == L"plays .fake and .fk2 files and reads their tags, adds the tool Echo and shows the visualizer Feed reader",
              L"a helper's summary says what it does: %ls", HelperSummary(h).c_str());
        Check(parse("[helper]\nformat = 2\n").error.find(L"later cordinal") != std::wstring::npos &&
                  parse("[decoder]\nextensions = fake\n").error.find(L"[helper]") != std::wstring::npos &&
                  parse(head).error.find(L"doesn't say") != std::wstring::npos,
              L"a helper written for a later cordinal, or one that says nothing, can't be used");
        const Helper noFile = parse(head + "[decoder]\nextensions = fake\ndecode = fake.exe --fake-helper wav\n");
        Check(noFile.decode.program.empty() && noFile.extensions.empty() && noFile.error == L"none of it can be used",
              L"a decoder must be told where the file goes");
        const Helper misplaced = parse(head + "[decoder]\nextensions = fake\ndecode = fake.exe {%title%} {file}\n"
                                              "info = fake.exe {feed} {file}\n"
                                              "[visualizer]\nlabel = Seeker\ncommand = fake.exe {feed} {start}\n");
        Check(misplaced.decode.program.empty() && misplaced.info.program.empty() && misplaced.extensions.empty() &&
                  misplaced.visualizers.empty() && misplaced.warnings.size() == 2 &&
                  misplaced.warnings[0].find(L"{%title%}, which cordinal doesn't fill in there") != std::wstring::npos,
              L"a placeholder is only filled in where it means something, and a command that uses one elsewhere never runs");
        std::filesystem::create_directories(scratch + L"\\helpers\\~staged", ec);
        std::filesystem::create_directories(scratch + L"\\helpers\\empty", ec);
        auto all = ReadHelpers(scratch + L"\\helpers");
        Check(all.size() == 2 && all[0].id == L"empty" && all[0].error.find(L"no helper.ini") != std::wstring::npos && all[1].id == L"fake",
              L"every helper folder is listed in name order, one without a helper.ini with the reason");
        Check(HelpersOffReason(true, false).find(L"safe mode") != std::wstring::npos &&
                  HelpersOffReason(false, true).find(L"administrator") != std::wstring::npos && HelpersOffReason(false, false).empty(),
              L"safe mode and an elevated cordinal start no helper program");
    }

    {
        testlog::Group group(L"helper command lines");
        const Helper h = parse(head + "[tool]\nlabel = Echo\ncommand = fake.exe --fake-helper echo {file} \"{%title% - x}\" --flag={file} \"\" {{literal}}\n");
        const std::wstring file = scratch + L"\\a folder\\track one.fake", title = L"He said \"hi\" \\ and \\\\\" end\\";
        HelperValues v;
        v.file = file;
        v.format = [&](const std::wstring& pattern) { return pattern == L"%title% - x" ? title + L" - x" : L"?"; };
        std::wstring error;
        bool complete = false;
        DWORD code = 0;
        auto lines = Lines(HelperText(ReadAllOf(h, HelperArguments(h.tools[0].command, v), error, complete, code)));
        Check(complete && code == 0 && lines.size() >= 5 && lines[0] == file && lines[1] == title + L" - x" &&
                  lines[2] == L"--flag=" + file && lines[3].empty() && lines[4] == L"{literal}",
              L"each value reaches the program as one argument, whatever quotes and backslashes it holds");
        HelperCommand all;
        all.args = {L"{files}", L"--then"};
        v.files = {L"one.fake", L"two words.fake"};
        Check(HelperArguments(all, v) == L"one.fake \"two words.fake\" --then", L"{files} gives every file its own argument");
    }

    {
        testlog::Group group(L"helper decoding");
        auto helper = std::make_shared<Helper>(parse(head + "[decoder]\nextensions = fake\n"
                                                            "decode = fake.exe --fake-helper wav 2 44100 2 16 1 {start} {file}\n"
                                                            "info = fake.exe --fake-helper info {file}\n"));
        Check(!IsSupportedAudioFile(song) && OpenFileFilterSpec().find(L"*.fake") == std::wstring::npos,
              L"a helper's file types are no audio until it is on");
        SetActiveHelpers({helper});
        Check(IsSupportedAudioFile(song) && IsSupportedAudioFile(song + L"|2") && OpenFileFilterSpec().find(L";*.fake") != std::wstring::npos &&
                  !HelperForExtension(L"flac"),
              L"a helper that is on adds its file types to the scanner and the open dialog");
        std::wstring error;
        auto d = OpenDecoder(song, 48000, error);
        bool exact = d && d->Rate() == 44100 && std::fabs(d->Duration() - 2.0) < 1e-9;
        std::vector<float> got;
        if (d) {
            std::vector<float> buffer(2 * 1500);
            while (size_t n = d->Read(buffer.data(), 1500)) got.insert(got.end(), buffer.begin(), buffer.begin() + n * 2);
            exact = exact && !d->Failed() && got.size() == 2 * 88200;
            for (size_t n = 0; exact && n < 88200; ++n)
                exact = got[n * 2] == Signal16(n, 0) / 32768.0f && got[n * 2 + 1] == Signal16(n, 1) / 32768.0f;
        }
        Check(exact, L"a helper's WAV output plays sample for sample, at its own rate and length: %ls", error.c_str());
        exact = d && d->Seek(1.25);
        float first[2]{};
        exact = exact && d->Read(first, 1) == 1 && first[0] == Signal16(55125, 0) / 32768.0f && first[1] == Signal16(55125, 1) / 32768.0f;
        Check(exact, L"seeking starts the helper again at {start}, on the right sample");
        d.reset();

        // Without {start}, cordinal reads from the top and skips ahead; a 24-bit mono stream is mixed to stereo.
        auto skipping = std::make_shared<Helper>(parse(head + "[decoder]\nextensions = fake\n"
                                                              "decode = fake.exe --fake-helper wav 1 8000 1 24 0 0 {file}\n"));
        SetActiveHelpers({skipping});
        d = OpenDecoder(song, 48000, error);
        exact = d && d->Duration() == 0 && d->Seek(0.5);
        float frame[2]{};
        exact = exact && d->Read(frame, 1) == 1 && frame[0] == Signal24(4000) / 8388608.0f && frame[1] == frame[0];
        Check(exact, L"a helper that can't seek is read from the top to the place asked for");
        d.reset();

        SetActiveHelpers({helper});
        TagData tags;
        Check(ReadTags(song, tags) && tags.title == "Fake Song" && tags.artist == "Someone" && tags.duration == 2.5 && tags.track == 3 &&
                  tags.trackTotal == 9 && tags.sampleRate == 44100 && tags.codec == Codec::Unknown,
              L"a helper's info command gives the file's tags, in cordinal's names or ffprobe's");
        TagEdit edit;
        std::vector<std::wstring> ignored;
        ParseHelperEdit("title=New Title\r\ngenre=\nnot a tag line\ntrack=7/12\nreplaygain_track_gain=-6.20 dB\nmood=calm\n", edit, ignored);
        Check(edit.title == std::optional<std::string>("New Title") && edit.genre == std::optional<std::string>("") && edit.track == 7u &&
                  edit.trackTotal == 12u && edit.rgTrackGain == std::optional<std::string>("-6.20 dB") && !edit.artist &&
                  ignored == std::vector<std::wstring>{L"not a tag line", L"mood=calm"},
              L"a tool's tags become a tag edit, an empty value removing its field, and other lines are named");
    }

    {
        testlog::Group group(L"helper failures");
        auto run = [&](const char* decode) {
            SetActiveHelpers({std::make_shared<Helper>(parse(head + "[decoder]\nextensions = fake\ndecode = fake.exe --fake-helper " +
                                                             decode + " {file}\n"))});
        };
        std::wstring error;
        run("crash 1000");
        auto d = OpenDecoder(song, 48000, error);
        std::vector<float> buffer(2 * 4096);
        size_t frames = 0;
        while (d)
            if (size_t n = d->Read(buffer.data(), 4096)) frames += n;
            else break;
        Check(d && frames == 1000 && d->Failed() && d->Problem() == L"the Fake helper stopped: it broke at frame 1000",
              L"a helper that stops plays what it wrote, then fails with the last thing it printed: %ls", d ? d->Problem().c_str() : L"");
        run("bad");
        d = OpenDecoder(song, 48000, error);
        Check(!d && error == L"Couldn't play song.fake: the Fake helper stopped: unsupported codec.",
              L"a helper whose output isn't WAV fails to open with what it printed: %ls", error.c_str());
        SetHelperPatience(400);
        run("hang");
        d = OpenDecoder(song, 48000, error);
        ULONGLONG started = GetTickCount64();
        Check(d && d->Read(buffer.data(), 4096) == 0 && d->Failed() && d->Problem() == L"the Fake helper stopped answering" &&
                  GetTickCount64() - started < 5000,
              L"a helper that goes quiet is given up on after the patience");
        run("silent");
        started = GetTickCount64();
        d = OpenDecoder(song, 48000, error);
        Check(!d && error == L"Couldn't play song.fake: the Fake helper didn't answer." && GetTickCount64() - started < 5000,
              L"a helper that never writes a header doesn't hold up the opening: %ls", error.c_str());
        SetHelperPatience(15000);
        run("linger");
        started = GetTickCount64();
        d = OpenDecoder(song, 48000, error);
        frames = 0;
        while (d)
            if (size_t n = d->Read(buffer.data(), 4096)) frames += n;
            else break;
        Check(d && frames == 100 && !d->Failed() && GetTickCount64() - started < 2500,
              L"a helper's output ends with the helper, though a program it started still holds its stdout");
        run("wav 600 44100 2 16 0 {start}");
        d = OpenDecoder(song, 48000, error);
        std::atomic<bool> aborting{false};
        std::thread abort([&] {
            Sleep(150);
            aborting = true;
            if (d) d->Abort();
        });
        started = GetTickCount64();
        while (d && d->Read(buffer.data(), 4096)) {}
        abort.join();
        Check(d && aborting && d->Failed() && GetTickCount64() - started < 5000, L"Abort ends a helper mid-stream");
        d.reset();
        HelperProcess sleeper;
        std::atomic<bool> cancel{false};
        std::string out;
        bool startedSleeping = sleeper.Start(dir + L"\\fake.exe", L"--fake-helper sleep 3000", dir, error);
        std::thread canceller([&] {
            Sleep(150);
            cancel = true;
        });
        started = GetTickCount64();
        bool complete = startedSleeping && sleeper.ReadAll(out, 1024, &cancel);
        canceller.join();
        Check(startedSleeping && !complete && GetTickCount64() - started < 2500 &&
                  WaitForSingleObject(sleeper.Process(), 2000) == WAIT_OBJECT_0,
              L"a tool that thinks quietly is waited for until it is cancelled, which ends it");
        Check(!HelperProcess().Start(dir + L"\\missing.exe", L"", dir, error) && !error.empty(),
              L"a program that can't start says why: %ls", error.c_str());
        SetActiveHelpers({});
    }

    {
        testlog::Group group(L"helper visual feed");
        HelperFeedWriter writer;
        bool open = writer.Open();
        writer.Publish([](HelperFeed& f) {
            f.state = 1;
            f.rate = 48000;
            f.clock = 12.5;
            f.levels[0] = 0.5f;
            f.wave[1] = -0.25f;
        });
        HANDLE reader = open ? writer.ReaderHandle() : nullptr;
        auto view = reader ? static_cast<const HelperFeed*>(MapViewOfFile(reader, FILE_MAP_READ, 0, 0, 0)) : nullptr;
        auto copy = std::make_unique<HelperFeed>();
        Check(view && ReadHelperFeed(view, *copy) && !std::memcmp(copy->magic, "CDVF", 4) && copy->version == 1 &&
                  copy->size == sizeof(HelperFeed) && copy->frames == HelperFeed::kFrames && copy->state == 1 && copy->rate == 48000 &&
                  copy->clock == 12.5 && copy->wave[1] == -0.25f && (copy->sequence & 1) == 0 && offsetof(HelperFeed, clock) % 8 == 0,
              L"the feed holds what cordinal published, with an even sequence once it is written");
        if (view) UnmapViewOfFile(view);
        std::wstring error;
        HelperProcess visualizer;
        std::string out;
        bool read = reader && visualizer.Start(dir + L"\\fake.exe", L"--fake-helper feed " + std::to_wstring((uintptr_t)reader), dir, error,
                                               reader) &&
                    visualizer.ReadAll(out, 1024) && visualizer.Finish(5000) == 0;
        Check(read && out == "CDVF 1 2048 40 1 48000 0.500 read-only\n",
              L"a visualizer maps the feed through the handle it is given, and only for reading: %hs", out.c_str());
        if (reader) CloseHandle(reader);
    }

    {
        testlog::Group group(L"helper trust");
        std::wstring record = MakeHelperTrust(dir), first = record;
        Check(!record.empty() && record.find(L"helper.ini") == std::wstring::npos && record.find(L"fake.exe*") != std::wstring::npos &&
                  CheckHelperTrust(dir, record) && record == first,
              L"a helper's record holds its programs' hashes and matches them while they are unchanged");
        WriteTestBytes(dir + L"\\helper.ini", head);
        std::wstring withIni = MakeHelperTrust(dir);
        HANDLE f = CreateFileW((dir + L"\\helper.ini").c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        FILETIME later{0x10000000, 0x01DC0000};
        bool touched = f != INVALID_HANDLE_VALUE && SetFileTime(f, nullptr, nullptr, &later);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        std::wstring updated = withIni;
        Check(touched && CheckHelperTrust(dir, updated) && updated != withIni && withIni.find(L"|helper.ini*") != std::wstring::npos,
              L"a file saved again with the same bytes still matches, and its record is brought up to date");
        WriteTestBytes(dir + L"\\helper.ini", head + "; changed\n");
        updated = withIni;
        Check(!CheckHelperTrust(dir, updated), L"a changed helper.ini no longer matches the record");
        WriteTestBytes(dir + L"\\helper.ini", head);
        withIni = MakeHelperTrust(dir);
        WriteTestBytes(dir + L"\\extra.dll", "MZ");
        updated = withIni;
        Check(!CheckHelperTrust(dir, updated), L"a new library in the helper's folder no longer matches the record");
        DeleteFileW((dir + L"\\extra.dll").c_str());
        HANDLE exe = CreateFileW((dir + L"\\fake.exe").c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD written = 0;
        bool grown = exe != INVALID_HANDLE_VALUE && WriteFile(exe, "!", 1, &written, nullptr) && written == 1;
        if (exe != INVALID_HANDLE_VALUE) CloseHandle(exe);
        updated = withIni;
        Check(grown && !CheckHelperTrust(dir, updated), L"a changed program no longer matches the record");
    }
}
