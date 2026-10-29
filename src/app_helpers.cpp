// Helpers in the running player (docs/plugins.md, "Helpers"; helpers.cpp has the parts that need no
// window). View > Helpers lists the helpers in <data>\helpers. Each is off until the user turns it
// on after reading which programs it runs; cordinal then keeps a record of its files in settings.ini
// ([Helpers] Trust.<id>) and turns it off again, saying so, when one of them changes. Edit > Run and
// the track list's right-click menu run the tools of the helpers that are on, one run at a time on a
// worker thread; View > Helpers also starts and stops their visualizers, which read the visual feed
// while they run. Safe mode (Shift at start, or --safe) and a cordinal running as administrator
// start no helper program at all.
#include "app.h"
#include "cue.h"
#include "helpers.h"

namespace app {

namespace {

constexpr UINT_PTR kFeedTimer = 148;  // on g.hwnd while a visualizer runs
constexpr size_t kMaxOutput = 16u << 20, kMaxCommandLine = 30000;

// One tool run on the selection, prepared on the UI thread so the worker needs nothing from `g`.
struct ToolJob {
    std::wstring label, program, dir;
    HelperTool::Output output = HelperTool::Text;
    std::vector<std::wstring> arguments;  // one command line per run
    std::vector<TrackId> ids;             // the track each run is for, or kNone
    std::vector<std::wstring> names;      // its file name, which heads its output
};

struct ToolDone {
    std::wstring label;
    HelperTool::Output output = HelperTool::Text;
    std::wstring text;
    std::vector<std::pair<TrackId, TagEdit>> edits;
    std::vector<std::wstring> ignored, failures;
    bool cancelled = false;
};

struct Shown {
    size_t helper, index;
    std::unique_ptr<HelperProcess> process;
};

struct State {
    std::vector<std::shared_ptr<Helper>> helpers;  // every helper folder, in name order
    std::vector<bool> on;
    std::vector<std::wstring> why;  // why a helper the user turned on is off now
    std::wstring off;               // why no helper runs at all, or empty
    std::vector<std::pair<size_t, size_t>> tools, visualizers;  // (helper, index), in menu order
    std::thread worker;
    std::atomic<bool> cancel{false};
    std::atomic<int> done{0}, total{0};
    std::wstring running;  // the label of the tool that runs
    int runningTool = -1;  // its place in s.tools, for its Cancel item
    std::vector<Shown> shown;
    HelperFeedWriter feed;
} s;

std::wstring HelpersDir() { return g.dataDir + L"\\helpers"; }

std::vector<std::wstring> SplitList(const std::wstring& text) {
    std::vector<std::wstring> out;
    for (size_t at = 0; at <= text.size();) {
        size_t bar = std::min(text.find(L'|', at), text.size());
        if (bar > at) out.push_back(text.substr(at, bar - at));
        at = bar + 1;
    }
    return out;
}

bool Listed(const std::vector<std::wstring>& list, const std::wstring& id) {
    return std::any_of(list.begin(), list.end(), [&](auto& x) { return _wcsicmp(x.c_str(), id.c_str()) == 0; });
}

std::wstring EscapeAmp(const std::wstring& text) {
    std::wstring out;
    for (wchar_t c : text) out += c == L'&' ? L"&&" : std::wstring(1, c);
    return out;
}

std::wstring TrustKey(const Helper& h) { return L"Trust." + h.id; }

// A trust record outgrows IniRead's 2047 characters for a helper with many libraries.
std::wstring ReadTrust(const Helper& h) {
    std::wstring record(1 << 16, L'\0');
    record.resize(GetPrivateProfileStringW(L"Helpers", TrustKey(h).c_str(), L"", record.data(), (DWORD)record.size(), g.iniPath.c_str()));
    return record;
}

void SaveOn() {
    std::wstring ids;
    for (size_t i = 0; i < s.helpers.size(); ++i)
        if (s.on[i]) ids += (ids.empty() ? L"" : L"|") + s.helpers[i]->id;
    IniWrite(L"Helpers", L"On", ids);
}

// The feed exists only while a visualizer runs.
void CloseFeedIfIdle() {
    if (!s.shown.empty()) return;
    if (g.hwnd) KillTimer(g.hwnd, kFeedTimer);
    s.feed.Close();
}

// Ends the visualizers of helper `helper`, or all of them.
void CloseVisualizers(size_t helper = SIZE_MAX) {
    std::erase_if(s.shown, [&](Shown& v) {
        if (helper != SIZE_MAX && v.helper != helper) return false;
        v.process->Stop();
        return true;
    });
    CloseFeedIfIdle();
}

// Hands the helpers that are on to the decoder, scanner and tag reader, and their tools and
// visualizers to the menus.
void ApplyHelpers() {
    std::vector<std::shared_ptr<const Helper>> active;
    s.tools.clear();
    s.visualizers.clear();
    for (size_t i = 0; i < s.helpers.size(); ++i) {
        if (!s.on[i]) continue;
        active.push_back(s.helpers[i]);
        for (size_t t = 0; t < s.helpers[i]->tools.size() && s.tools.size() < (size_t)kMaxHelperTools; ++t) s.tools.push_back({i, t});
        for (size_t v = 0; v < s.helpers[i]->visualizers.size() && s.visualizers.size() < (size_t)kMaxHelperVisualizers; ++v)
            s.visualizers.push_back({i, v});
    }
    SetActiveHelpers(std::move(active));
    if (!g.hwnd) return;
    HMENU old = MainMenuBar();
    ReplaceMenuBar(BuildMenu());
    if (old) DestroyMenu(old);
    if (g.accel) DestroyAcceleratorTable(g.accel);
    g.accel = BuildAccelerators();  // a pack's key may name a helper's tool
}

// Reads the helpers folder; returns how many helpers it holds that earlier runs had not seen. A
// helper that is on but whose files changed since the user turned it on is turned off.
int ReadAll(std::vector<std::wstring>& changed) {
    CloseVisualizers();
    auto found = ReadHelpers(HelpersDir());
    if (found.size() > (size_t)kMaxHelpers) found.resize(kMaxHelpers);
    auto onList = SplitList(IniRead(L"Helpers", L"On", L"")), known = SplitList(IniRead(L"Helpers", L"Known", L""));
    s.helpers.clear();
    s.on.clear();
    s.why.clear();
    int fresh = 0;
    std::wstring ids;
    for (auto& h : found) {
        bool on = h.error.empty() && Listed(onList, h.id);
        std::wstring why;
        if (on) {
            std::wstring record = ReadTrust(h), before = record;
            if (!CheckHelperTrust(h.dir, record)) {
                on = false;
                why = L"changed since you turned it on";
                changed.push_back(h.name);
            } else if (record != before) {
                IniWrite(L"Helpers", TrustKey(h).c_str(), record);
            }
        }
        fresh += !Listed(known, h.id);
        ids += (ids.empty() ? L"" : L"|") + h.id;
        s.helpers.push_back(std::make_shared<Helper>(std::move(h)));
        s.on.push_back(on);
        s.why.push_back(why);
    }
    IniWrite(L"Helpers", L"Known", ids);
    if (!changed.empty()) SaveOn();
    return fresh;
}

std::wstring ChangedText(const std::vector<std::wstring>& changed) {
    const std::wstring them = changed.size() == 1 ? L"it" : L"them";
    return (changed.size() == 1 ? changed[0] + L" changed" : std::to_wstring(changed.size()) + L" helpers changed") +
           L" since you turned " + them + L" on, so cordinal turned " + them + L" off. Turn " + them +
           L" on again in View > Helpers if you trust the new version.";
}

void ReloadHelpers() {
    if (!s.off.empty()) {
        ShowMessage(L"Helpers are " + s.off + L".");
        return;
    }
    std::vector<std::wstring> changed;
    ReadAll(changed);
    ApplyHelpers();
    size_t on = std::count(s.on.begin(), s.on.end(), true);
    ShowMessage(changed.empty() ? std::to_wstring(s.helpers.size()) + (s.helpers.size() == 1 ? L" helper, " : L" helpers, ") +
                                      std::to_wstring(on) + L" on."
                                : ChangedText(changed));
}

void ToggleHelper(size_t i) {
    const Helper& h = *s.helpers[i];
    if (!h.error.empty() || !s.off.empty()) return;
    if (s.on[i]) {
        s.on[i] = false;
        SaveOn();
        IniWrite(L"Helpers", TrustKey(h).c_str(), L"");
        CloseVisualizers(i);
        ApplyHelpers();
        ShowMessage(L"Turned off " + h.name + L".");
        return;
    }
    std::wstring text = L"Turn on the helper \"" + h.name + L"\"" + (h.version.empty() ? L"" : L" " + h.version) +
                        (h.author.empty() ? L"" : L" by " + h.author) + L"?\n\nIt " + HelperSummary(h) + L".";
    if (!h.description.empty()) text += L"\n\n" + h.description;
    text += L"\n\nIt runs " + HelperProgramList(h) +
            L" from its folder as separate programs, with your permissions. cordinal can't check what they do, so turn "
            L"on only helpers you trust. If its helper.ini or any of its programs changes, cordinal turns it off until "
            L"you turn it on again.";
    if (!h.warnings.empty())
        text += L"\n\n" + std::to_wstring(h.warnings.size()) + (h.warnings.size() == 1 ? L" thing" : L" things") +
                L" in it will be left out (View > Helpers > What helpers left out).";
    if (MessageBoxW(g.hwnd, text.c_str(), L"Turn on a helper", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    HCURSOR cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring record = MakeHelperTrust(h.dir);  // a large program takes a moment to hash
    SetCursor(cursor);
    if (record.empty()) {
        ShowMessage(L"Couldn't read the files of " + h.name + L", so it stays off.");
        return;
    }
    IniWrite(L"Helpers", TrustKey(h).c_str(), record);
    s.on[i] = true;
    s.why[i].clear();
    SaveOn();
    ApplyHelpers();
    // Files of its types were no audio to the scanner until now.
    if (!h.extensions.empty() && !g.libraryFolders.empty()) g.scanner.RescanAll();
    ShowMessage(L"Turned on " + h.name + L"." +
                (!h.extensions.empty() && !g.libraryFolders.empty() ? L" Looking for its files in your library folders." : L""));
}

void ShowReport() {
    std::wstring text;
    if (!s.off.empty()) text = L"Helpers are " + s.off + L".\n";
    for (size_t i = 0; i < s.helpers.size(); ++i) {
        const Helper& h = *s.helpers[i];
        if (h.error.empty() && h.warnings.empty() && s.why[i].empty()) continue;
        text += h.name + (s.on[i] ? L"" : L" (off)") + L"\n";
        if (!h.error.empty()) text += L"    Can't be used: " + h.error + L".\n";
        if (!s.why[i].empty()) text += L"    Off: " + s.why[i] + L".\n";
        for (const auto& w : h.warnings) text += L"    " + w + L"\n";
    }
    // The first helper that is on wins a file type that two of them claim.
    for (size_t i = 0; i < s.helpers.size(); ++i)
        for (const auto& e : s.helpers[i]->extensions)
            if (s.on[i] && HelperForExtension(e) != s.helpers[i])
                text += s.helpers[i]->name + L": ." + e + L" files go to " + HelperForExtension(e)->name + L", which is earlier in the list.\n";
    if (text.empty()) text = s.helpers.empty() ? L"There are no helpers in " + HelpersDir() + L"." : L"Every helper was read in full.";
    MessageBoxW(g.hwnd, text.c_str(), L"Helpers", MB_OK | MB_ICONINFORMATION);
}

void ToolWorker(HWND hwnd, ToolJob job) {
    auto* done = new ToolDone();
    done->label = job.label;
    done->output = job.output;
    for (size_t i = 0; i < job.arguments.size(); ++i) {
        HelperProcess p;
        std::wstring error;
        std::string out;
        const bool started = p.Start(job.program, job.arguments[i], job.dir, error);
        const bool complete = started && p.ReadAll(out, kMaxOutput, &s.cancel);
        if (s.cancel) {
            done->cancelled = true;
            break;
        }
        const DWORD code = complete ? p.Finish(10000) : ERROR_CANCELLED;
        const std::wstring name = job.names[i].empty() ? L"" : job.names[i] + L": ";
        if (!started) done->failures.push_back(name + L"couldn't start: " + error);
        else if (!complete) done->failures.push_back(name + L"it printed more than 16 MB");
        else if (code != 0 && code != ERROR_TIMEOUT) {  // one that closed its output but lingers is done
            std::wstring said = p.Complaint();
            done->failures.push_back(name + (said.empty() ? L"it stopped with code " + std::to_wstring((int)code) : said));
        } else if (job.output == HelperTool::Tags) {
            TagEdit edit;
            ParseHelperEdit(out, edit, done->ignored);
            if (!edit.Empty()) done->edits.emplace_back(job.ids[i], std::move(edit));
        } else if (job.output == HelperTool::Text) {
            // Each track's output under its file name; a run for the whole selection prints its own.
            std::wstring text = HelperText(out);
            while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
            std::wstring part = job.names[i].empty() ? text : job.names[i] + L"\n" + text;
            if (!part.empty()) done->text += (done->text.empty() ? L"" : L"\n\n") + part;
        }
        ++s.done;
        PostMessageW(hwnd, WM_APP_HELPER, 0, 0);
    }
    if (!PostMessageW(hwnd, WM_APP_HELPER, 1, (LPARAM)done)) delete done;
}

void RunTool(size_t k) {
    if (!s.running.empty()) {
        ShowMessage(s.running + L" is still running. Wait for it, or cancel it in the Run menu.");
        return;
    }
    const Helper& h = *s.helpers[s.tools[k].first];
    const HelperTool& t = h.tools[s.tools[k].second];
    auto ids = SelectedTracks();
    if ((t.perTrack || t.together) && ids.empty()) {
        ShowMessage(L"Select the tracks to run " + t.label + L" on.");
        return;
    }
    ToolJob job;
    job.label = t.label;
    job.program = h.dir + L"\\" + t.command.program;
    job.dir = h.dir;
    job.output = t.output;
    auto add = [&](const std::wstring& arguments, TrackId id, const std::wstring& name) {
        job.arguments.push_back(arguments);
        job.ids.push_back(id);
        job.names.push_back(name);
    };
    if (t.together) {
        // One run for the whole selection, or a few when its paths don't fit one command line.
        HelperValues v;
        size_t length = 0;
        for (TrackId id : ids) {
            std::wstring file = AudioFileOf(g_lib.Path(id));
            if (std::find(v.files.begin(), v.files.end(), file) != v.files.end()) continue;  // CUE tracks of one file
            if (!v.files.empty() && length + file.size() + 3 > kMaxCommandLine) {
                add(HelperArguments(t.command, v), kNone, L"");
                v.files.clear();
                length = 0;
            }
            v.files.push_back(file);
            length += file.size() + 3;
        }
        add(HelperArguments(t.command, v), kNone, L"");
    } else if (t.perTrack) {
        for (TrackId id : ids) {
            HelperValues v;
            v.file = AudioFileOf(g_lib.Path(id));
            v.format = [id](const std::wstring& pattern) { return FormatTitle(pattern, Lookup(id, false)); };
            add(HelperArguments(t.command, v), id, FileNameOf(v.file));
        }
    } else {
        add(HelperArguments(t.command, {}), kNone, L"");
    }
    if (s.worker.joinable()) s.worker.join();
    s.cancel = false;
    s.done = 0;
    s.total = (int)job.arguments.size();
    s.running = t.label;
    s.runningTool = (int)k;
    s.worker = std::thread(ToolWorker, g.hwnd, std::move(job));
    UpdateStatus();
}

// "title = Dreams, track = 3" for a proposal's line.
std::wstring Describe(const TagEdit& e) {
    std::wstring out;
    auto text = [&](const wchar_t* name, const std::optional<std::string>& v) {
        if (!v) return;
        std::wstring value = FromUtf8(*v);
        if (value.size() > 40) value = value.substr(0, 39) + L"\x2026";
        out += (out.empty() ? L"" : L", ") + std::wstring(name) + (value.empty() ? L" removed" : L" = " + value);
    };
    auto number = [&](const wchar_t* name, const std::optional<uint32_t>& v) {
        if (v) out += (out.empty() ? L"" : L", ") + std::wstring(name) + (*v ? L" = " + std::to_wstring(*v) : L" removed");
    };
    text(L"title", e.title), text(L"artist", e.artist), text(L"album", e.album), text(L"album artist", e.albumArtist);
    text(L"genre", e.genre), text(L"date", e.date), text(L"composer", e.composer), text(L"comment", e.comment);
    text(L"grouping", e.grouping), text(L"label", e.label), text(L"lyrics", e.lyrics);
    number(L"track", e.track), number(L"tracks", e.trackTotal), number(L"disc", e.disc), number(L"discs", e.discTotal);
    number(L"BPM", e.bpm);
    text(L"track gain", e.rgTrackGain), text(L"track peak", e.rgTrackPeak), text(L"album gain", e.rgAlbumGain);
    text(L"album peak", e.rgAlbumPeak);
    return out;
}

std::wstring Failures(const ToolDone& d) {
    if (d.failures.empty()) return L"";
    std::wstring text = L"It couldn't finish on " + std::to_wstring(d.failures.size()) + (d.failures.size() == 1 ? L" run:" : L" runs:");
    for (size_t i = 0; i < d.failures.size() && i < 8; ++i) text += L"\n    " + d.failures[i];
    if (d.failures.size() > 8) text += L"\n    and " + std::to_wstring(d.failures.size() - 8) + L" more";
    return text;
}

// A tool's tags are a proposal: the user sees them before the tag writer (atomic and undoable)
// changes any file.
void ProposeTags(ToolDone& d) {
    if (d.edits.empty()) {
        std::wstring failures = Failures(d);
        if (failures.empty()) ShowMessage(d.label + L" proposed no tag changes.");
        else MessageBoxW(g.hwnd, (d.label + L" proposed no tag changes.\n\n" + failures).c_str(), d.label.c_str(), MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring text = d.label + L" proposes tag changes for " + std::to_wstring(d.edits.size()) +
                        (d.edits.size() == 1 ? L" track:\n" : L" tracks:\n");
    for (size_t i = 0; i < d.edits.size() && i < 8; ++i)
        text += L"\n" + FileNameOf(AudioFileOf(g_lib.Path(d.edits[i].first))) + L": " + Describe(d.edits[i].second);
    if (d.edits.size() > 8) text += L"\nand " + std::to_wstring(d.edits.size() - 8) + L" more";
    if (!d.ignored.empty())
        text += L"\n\nIt also printed " + std::to_wstring(d.ignored.size()) + (d.ignored.size() == 1 ? L" line" : L" lines") +
                L" that cordinal doesn't take as tags, such as: " + d.ignored[0];
    if (!d.failures.empty()) text += L"\n\n" + Failures(d);
    text += L"\n\nWrite these tags? Edit > Undo last tag edit puts the old ones back.";
    if (MessageBoxW(g.hwnd, text.c_str(), L"Proposed tags", MB_YESNO | MB_ICONQUESTION) == IDYES) SubmitTagWrites(d.edits, true, true);
}

// Short output in a message box; longer output in a text file in <data>\reports, opened in the
// program Windows uses for text.
void ShowOutput(const ToolDone& d) {
    std::wstring text = d.text, failures = Failures(d);
    if (!failures.empty()) text += (text.empty() ? L"" : L"\n\n") + failures;
    if (text.empty()) {
        ShowMessage(d.label + L" finished and printed nothing.");
        return;
    }
    if (std::count(text.begin(), text.end(), L'\n') < 20 && text.size() <= 2000) {
        MessageBoxW(g.hwnd, text.c_str(), d.label.c_str(), MB_OK | (d.failures.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
        return;
    }
    const std::wstring dir = g.dataDir + L"\\reports";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamp[40];
    swprintf_s(stamp, L" %04u-%02u-%02u %02u.%02u.%02u.txt", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::wstring name = d.label;
    for (auto& c : name)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    const std::wstring file = dir + L"\\" + name + stamp;
    std::wstring lines;  // Windows line ends, whatever the program printed
    for (wchar_t c : text)
        if (c != L'\r') lines += c == L'\n' ? L"\r\n" : std::wstring(1, c);
    std::string bytes = "\xEF\xBB\xBF" + ToUtf8(lines);
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    if (f != INVALID_HANDLE_VALUE) ok = CloseHandle(f) && ok;
    if (!ok) {
        ShowMessage(L"Couldn't save the output of " + d.label + L" in " + dir + L".");
        return;
    }
    ShellExecuteW(g.hwnd, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ShowMessage(d.label + L" finished. Its output is in " + file + L".");
}

void ToggleVisualizer(size_t k) {
    const auto [hi, vi] = s.visualizers[k];
    const HelperVisualizer& v = s.helpers[hi]->visualizers[vi];
    auto it = std::find_if(s.shown.begin(), s.shown.end(), [&](const Shown& x) { return x.helper == hi && x.index == vi; });
    if (it != s.shown.end()) {
        it->process->Stop();
        s.shown.erase(it);
        CloseFeedIfIdle();
        ShowMessage(L"Closed " + v.label + L".");
        return;
    }
    // The visualizer gets a read-only handle to the feed, under the number {feed} names.
    HANDLE reader = s.feed.Open() ? s.feed.ReaderHandle() : nullptr;
    HelperValues values;
    values.feed = std::to_wstring((uintptr_t)reader);
    auto process = std::make_unique<HelperProcess>();
    std::wstring error = L"cordinal couldn't share what is playing";
    const Helper& h = *s.helpers[hi];
    bool started = reader && process->Start(h.dir + L"\\" + v.command.program, HelperArguments(v.command, values), h.dir, error, reader, true);
    if (reader) CloseHandle(reader);  // the visualizer has its own copy
    if (!started) {
        CloseFeedIfIdle();
        ShowMessage(L"Couldn't start " + v.label + L": " + error + L".");
        return;
    }
    s.shown.push_back({hi, vi, std::move(process)});
    SetTimer(g.hwnd, kFeedTimer, 16, nullptr);
    ShowMessage(L"Started " + v.label + L".");
}

// What is playing, about 60 times a second: the samples around the audible clock and their spectrum,
// as the built-in visualizers see them. Paused playback keeps its last frame.
void PublishFeed() {
    const bool playing = g.playing != kNone, paused = playing && g.engine.Paused();
    s.feed.Publish([&](HelperFeed& f) {
        f.state = !playing ? 0 : paused ? 2 : 1;
        f.rate = g.engine.Rate();
        f.position = playing ? g.engine.Position() : 0;
        f.duration = playing ? g.engine.Duration() : 0;
        const Palette& p = P();
        const Rgb colors[] = {p.bg, p.text, p.sub, p.accent};
        std::copy(std::begin(colors), std::end(colors), f.colors);
        if (paused) return;
        double clock = -1;
        if (playing && g.engine.VisualSamples(f.wave, HelperFeed::kFrames, clock) == HelperFeed::kFrames) {
            f.clock = clock;
            SpectrumLevels(f.wave, f.rate, f.levels, HelperFeed::kBands);
        } else {
            f.clock = -1;
            std::fill(std::begin(f.wave), std::end(f.wave), 0.0f);
            std::fill(std::begin(f.levels), std::end(f.levels), 0.0f);
        }
    });
}

}  // namespace

void LoadHelpers(bool safe) {
    CreateDirectoryW(HelpersDir().c_str(), nullptr);
    s.off = HelpersOffReason(safe, ProcessElevated());
    std::vector<std::wstring> changed;
    int fresh = 0;
    if (s.off.empty()) {
        fresh = ReadAll(changed);
    } else {
        CloseVisualizers();
        s.helpers.clear();
        s.on.clear();
        s.why.clear();
    }
    ApplyHelpers();
    std::wstring message = !changed.empty() ? ChangedText(changed)
                           : fresh == 1     ? L"Found a new helper. Turn it on in View > Helpers."
                           : fresh > 1      ? L"Found new helpers. Turn them on in View > Helpers."
                                            : L"";
    if (!message.empty() && (g.message.empty() || !changed.empty())) {
        g.message = message;
        g.messageUntil = GetTickCount64() + 15000;
    }
}

HMENU HelpersMenu() {
    HMENU m = CreatePopupMenu();
    if (!s.off.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"Helpers are " + s.off).c_str());
    for (size_t i = 0; i < s.helpers.size(); ++i) {
        const Helper& h = *s.helpers[i];
        std::wstring label = EscapeAmp(h.name) + (h.version.empty() ? L"" : L" " + EscapeAmp(h.version));
        if (!h.error.empty()) label += L"  (can't be used: " + EscapeAmp(h.error) + L")";
        else if (!s.why[i].empty()) label += L"  (" + s.why[i] + L")";
        AppendMenuW(m, MF_STRING | (s.on[i] ? MF_CHECKED : 0) | (h.error.empty() ? 0 : MF_GRAYED), IDM_HELPER_BASE + i, label.c_str());
    }
    if (s.helpers.empty() && s.off.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No helpers yet");
    if (!s.visualizers.empty()) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        for (size_t k = 0; k < s.visualizers.size(); ++k)
            AppendMenuW(m, MF_STRING, IDM_HELPER_VIS_BASE + k,
                        EscapeAmp(s.helpers[s.visualizers[k].first]->visualizers[s.visualizers[k].second].label).c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_HELPERS_FOLDER, L"Open the helpers &folder");
    AppendMenuW(m, MF_STRING, IDM_HELPERS_RELOAD, L"&Reload helpers");
    AppendMenuW(m, MF_STRING, IDM_HELPERS_REPORT, L"What helpers &left out...");
    return m;
}

void AppendHelperTools(HMENU menu, bool selected) {
    if (s.tools.empty()) return;
    HMENU sub = CreatePopupMenu();
    for (size_t k = 0; k < s.tools.size(); ++k) {
        const HelperTool& t = s.helpers[s.tools[k].first]->tools[s.tools[k].second];
        const bool usable = s.running.empty() && (selected || !(t.perTrack || t.together));
        AppendMenuW(sub, MF_STRING | (usable ? 0 : MF_GRAYED), IDM_HELPER_TOOL_BASE + k, EscapeAmp(t.label).c_str());
        if ((int)k == s.runningTool) AppendMenuW(sub, MF_STRING, IDM_HELPER_CANCEL, HelperCancelText().c_str());
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"Ru&n");
}

int HelperToolIndex() { return s.runningTool; }

std::wstring HelperCancelText() { return L"&Cancel " + EscapeAmp(s.running); }

void UpdateHelperMenu(HMENU menu) {
    const bool selected = !SelectedTracks().empty();
    for (size_t k = 0; k < s.tools.size(); ++k) {
        const HelperTool& t = s.helpers[s.tools[k].first]->tools[s.tools[k].second];
        const bool usable = s.running.empty() && (selected || !(t.perTrack || t.together));
        EnableMenuItem(menu, IDM_HELPER_TOOL_BASE + (UINT)k, MF_BYCOMMAND | (usable ? MF_ENABLED : MF_GRAYED));
    }
    for (size_t k = 0; k < s.visualizers.size(); ++k) {
        bool shown = std::any_of(s.shown.begin(), s.shown.end(), [&](const Shown& v) {
            return v.helper == s.visualizers[k].first && v.index == s.visualizers[k].second;
        });
        CheckMenuItem(menu, IDM_HELPER_VIS_BASE + (UINT)k, MF_BYCOMMAND | (shown ? MF_CHECKED : MF_UNCHECKED));
    }
}

bool HelperMenuCommand(int id) {
    if (id >= IDM_HELPER_BASE && id < IDM_HELPER_BASE + (int)s.helpers.size()) ToggleHelper(id - IDM_HELPER_BASE);
    else if (id >= IDM_HELPER_TOOL_BASE && id < IDM_HELPER_TOOL_BASE + (int)s.tools.size()) RunTool(id - IDM_HELPER_TOOL_BASE);
    else if (id >= IDM_HELPER_VIS_BASE && id < IDM_HELPER_VIS_BASE + (int)s.visualizers.size()) ToggleVisualizer(id - IDM_HELPER_VIS_BASE);
    else if (id == IDM_HELPER_CANCEL) {
        if (s.running.empty()) return true;
        s.cancel = true;
        ShowMessage(L"Cancelling " + s.running + L"...");
    } else if (id == IDM_HELPERS_FOLDER) ShellExecuteW(g.hwnd, L"open", HelpersDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (id == IDM_HELPERS_RELOAD) ReloadHelpers();
    else if (id == IDM_HELPERS_REPORT) ShowReport();
    else return false;
    return true;
}

std::wstring HelperToolRunning() { return s.running; }

std::wstring HelperToolStatus() {
    return L"Running " + s.running + (s.total > 1 ? L" " + std::to_wstring(s.done.load()) + L"/" + std::to_wstring(s.total.load()) : L"");
}

void OnHelperMessage(WPARAM wp, LPARAM lp) {
    if (wp != 1) {
        UpdateStatus();
        return;
    }
    std::unique_ptr<ToolDone> done(reinterpret_cast<ToolDone*>(lp));
    if (s.worker.joinable()) s.worker.join();
    s.running.clear();
    s.runningTool = -1;
    s.total = 0;
    UpdateStatus();
    if (done->cancelled) ShowMessage(L"Cancelled " + done->label + L".");
    else if (done->output == HelperTool::Tags) ProposeTags(*done);
    else if (done->output == HelperTool::Text) ShowOutput(*done);
    else if (done->failures.empty()) ShowMessage(done->label + L" finished.");
    else MessageBoxW(g.hwnd, (done->label + L" finished.\n\n" + Failures(*done)).c_str(), done->label.c_str(), MB_OK | MB_ICONWARNING);
}

bool HelperTimer(UINT_PTR id) {
    if (id != kFeedTimer) return false;
    std::erase_if(s.shown, [](const Shown& v) { return !v.process->Running(); });  // closed by the user
    if (s.shown.empty()) CloseFeedIfIdle();
    else PublishFeed();
    return true;
}

std::wstring HelperDecoding(TrackId id) {
    if (id >= g_lib.Count()) return L"";
    auto helper = HelperForExtension(ExtOf(AudioFileOf(g_lib.Path(id))));
    return helper ? helper->name : L"";
}

void StopHelpers() {
    s.cancel = true;
    StopHelperPrograms();  // the tool, the visualizers, and any decoder a worker or the engine still reads
    if (s.worker.joinable()) s.worker.join();
    MSG pending;
    while (g.hwnd && PeekMessageW(&pending, g.hwnd, WM_APP_HELPER, WM_APP_HELPER, PM_REMOVE))
        if (pending.wParam == 1) delete reinterpret_cast<ToolDone*>(pending.lParam);
    s.running.clear();
    s.runningTool = -1;
    CloseVisualizers();
}

}  // namespace app
