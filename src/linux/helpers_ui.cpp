// Helpers in the running Linux player (docs/plugins.md, "Helpers"; helpers.cpp has the parts that need no
// window), as the Windows build's app_helpers.cpp for what this build runs: View > Helpers lists the helpers in
// ~/.local/share/cordinal/helpers. Each is off until the user turns it on after reading which programs it runs;
// cordinal then keeps a record of its files in settings.ini and turns it off again, saying so, when one of them
// changes. Edit > Run and the track list's right-click menu run the tools of the helpers that are on, one run at a
// time on a worker thread. A program is any executable file in the helper's folder, started with posix_spawn in
// a process group of its own that closing cordinal ends. Visualizers inherit a read-only visual-feed fd;
// the feed and its private writer exist only while a visualizer runs. A root process starts no helper program.
#include "app.h"
#include "cue.h"
#include "helpers.h"
#include "ui_internal.h"
#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace lx {
namespace {

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

struct ShownVisualizer {
    size_t helper, index;
    std::unique_ptr<HelperProcess> process;
};
struct State {
    std::vector<std::shared_ptr<Helper>> helpers;  // every helper folder, in name order
    std::vector<bool> on;
    std::vector<std::wstring> why;  // why a helper the user turned on is off now
    std::wstring off;               // why no helper runs at all, or empty
    std::vector<std::pair<size_t, size_t>> tools;  // (helper, index), in menu order
    std::vector<std::pair<size_t, size_t>> visualizers;
    std::vector<ShownVisualizer> shown;
    HelperFeedWriter feed;
    uint64_t feedAt = 0;
    std::thread worker;
    std::atomic<bool> cancel{false};
    std::wstring running;  // the label of the tool that runs
    int runningTool = -1;
} s;

fs::path HelpersDir() { return fs::path(compat::Narrow(g.dataDir.c_str())) / "helpers"; }

std::wstring TrustKey(const Helper& h) { return h.id; }

std::vector<std::wstring> SplitList(const std::string& text) {
    std::vector<std::wstring> out;
    for (size_t i = 0; i < text.size();) {
        size_t end = text.find('|', i);
        if (end == std::string::npos) end = text.size();
        if (end > i) out.push_back(FromUtf8(text.substr(i, end - i)));
        i = end + 1;
    }
    return out;
}

void SaveOn() {
    std::string list;
    for (size_t i = 0; i < s.helpers.size(); ++i)
        if (s.on[i]) list += (list.empty() ? "" : "|") + ToUtf8(s.helpers[i]->id);
    g.settings.helpersOn = list;
    SaveSettings();
}

void CloseHelperVisualizers(size_t helper = SIZE_MAX) {
    std::erase_if(s.shown, [&](auto& shown) {
        if (helper != SIZE_MAX && shown.helper != helper) return false;
        shown.process->Stop();
        return true;
    });
    if (s.shown.empty()) s.feed.Close();
}

// Hands the helpers that are on to the decoder, scanner and tag reader, and their tools to the menus.
void ApplyHelpers() {
    std::vector<std::shared_ptr<const Helper>> on;
    s.tools.clear();
    s.visualizers.clear();
    for (size_t i = 0; i < s.helpers.size(); ++i) {
        if (!s.on[i] || !s.off.empty()) continue;
        on.push_back(s.helpers[i]);
        for (size_t t = 0; t < s.helpers[i]->tools.size(); ++t) s.tools.push_back({i, t});
        for (size_t v = 0; v < s.helpers[i]->visualizers.size(); ++v) s.visualizers.push_back({i, v});
    }
    SetActiveHelpers(std::move(on));
    g.redraw = true;
}

}  // namespace

// Reads the helpers folder; a helper that is on but whose files changed since the user turned it on is turned off.
void LoadHelpers() {
    CloseHelperVisualizers();
    std::error_code ec;
    fs::create_directories(HelpersDir(), ec);
    s.off = HelpersOffReason(false, ProcessElevated());
    s.helpers.clear();
    for (Helper& h : ReadHelpers(HelpersDir().wstring())) s.helpers.push_back(std::make_shared<Helper>(std::move(h)));
    s.on.assign(s.helpers.size(), false);
    s.why.assign(s.helpers.size(), L"");
    const auto on = SplitList(g.settings.helpersOn);
    std::vector<std::wstring> changed;
    for (size_t i = 0; i < s.helpers.size(); ++i) {
        const Helper& h = *s.helpers[i];
        if (std::find(on.begin(), on.end(), h.id) == on.end() || !h.error.empty()) continue;
        std::wstring record = FromUtf8(g.settings.helperTrust[ToUtf8(TrustKey(h))]);
        if (s.off.empty() && !CheckHelperTrust(h.dir, record)) {
            s.why[i] = L"its files changed since you turned it on";
            changed.push_back(h.name);
            continue;
        }
        g.settings.helperTrust[ToUtf8(TrustKey(h))] = ToUtf8(record);
        s.on[i] = true;
    }
    if (!changed.empty()) {
        std::wstring names;
        for (auto& n : changed) names += (names.empty() ? L"" : L", ") + n;
        ShowMessage(L"Off until you turn them on again, because their files changed: " + names + L".");
        SaveOn();
    }
    ApplyHelpers();
}

size_t HelperCount() { return s.helpers.size(); }

HelperRow HelperAt(size_t i) {
    HelperRow row;
    if (i >= s.helpers.size()) return row;
    const Helper& h = *s.helpers[i];
    row.name = h.name.empty() ? h.id : h.name;
    row.version = h.version;
    row.author = h.author;
    row.description = h.description;
    row.summary = h.error.empty() ? HelperSummary(h) : L"";
    row.programs = HelperProgramList(h);
    row.error = h.error;
    row.warnings = h.warnings;
    row.why = s.why[i];
    row.on = s.on[i];
    row.offAll = s.off;
    return row;
}

// Turning one on asks first, saying which programs it runs and that they run with the user's permissions.
void ToggleHelper(size_t i) {
    if (i >= s.helpers.size()) return;
    const Helper& h = *s.helpers[i];
    if (!h.error.empty() || !s.off.empty()) return;
    if (s.on[i]) {
        CloseHelperVisualizers(i);
        s.on[i] = false;
        g.settings.helperTrust.erase(ToUtf8(TrustKey(h)));
    } else {
        std::wstring ask = L"Turn on " + (h.name.empty() ? h.id : h.name) + L"?\n\nIt " + HelperSummary(h) + L".\n\nIt runs " +
                           HelperProgramList(h) + L" from " + h.dir + L", with your permissions: only turn on helpers you trust.";
        if (!Confirm(ToUtf8(ask), "Turn on helper")) return;
        std::wstring record = MakeHelperTrust(h.dir);
        if (record.empty()) {
            ShowMessage(L"Couldn't read the helper's files to remember them.");
            return;
        }
        g.settings.helperTrust[ToUtf8(TrustKey(h))] = ToUtf8(record);
        s.on[i] = true;
        s.why[i].clear();
    }
    SaveOn();
    ApplyHelpers();
}

void OpenHelpersFolder() {
    std::error_code ec;
    fs::create_directories(HelpersDir(), ec);
    std::string url = "file://" + HelpersDir().string();
    SDL_OpenURL(url.c_str());
}

bool InstallHelper(const std::wstring& source) {
    std::error_code ec;
    fs::path from(compat::Narrow(source.c_str()));
    if (!fs::is_directory(from, ec) || !fs::exists(from / "helper.ini", ec)) {
        ShowMessage(L"A helper is a folder with a helper.ini in it.");
        return false;
    }
    fs::path target = HelpersDir() / from.filename();
    if (fs::exists(target, ec)) {
        ShowMessage(L"A helper of that name is already installed.");
        return false;
    }
    fs::copy(from, target, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) {
        ShowMessage(L"Couldn't install the helper.");
        return false;
    }
    LoadHelpers();
    ShowMessage(L"Installed the helper. Turn it on in View > Helpers.");
    return true;
}

std::vector<std::wstring> HelperToolLabels() {
    std::vector<std::wstring> labels;
    for (auto& [h, t] : s.tools) labels.push_back(s.helpers[h]->tools[t].label);
    return labels;
}

std::vector<std::wstring> HelperVisualizerLabels() {
    std::vector<std::wstring> labels;
    for (const auto& [h, v] : s.visualizers) labels.push_back(s.helpers[h]->visualizers[v].label);
    return labels;
}

bool HelperVisualizerRunning(size_t index) {
    if (index >= s.visualizers.size()) return false;
    const auto [h, v] = s.visualizers[index];
    return std::any_of(s.shown.begin(), s.shown.end(), [&](const auto& shown) { return shown.helper == h && shown.index == v; });
}

bool HelperVisualizersRunning() { return !s.shown.empty(); }

void ToggleHelperVisualizer(size_t index) {
    if (index >= s.visualizers.size() || !s.off.empty()) return;
    const auto [h, v] = s.visualizers[index];
    const auto& helper = *s.helpers[h];
    const auto& visualizer = helper.visualizers[v];
    auto shown = std::find_if(s.shown.begin(), s.shown.end(), [&](const auto& old) { return old.helper == h && old.index == v; });
    if (shown != s.shown.end()) {
        shown->process->Stop();
        s.shown.erase(shown);
        if (s.shown.empty()) s.feed.Close();
        ShowMessage(L"Closed " + visualizer.label + L".");
        return;
    }
    if (!s.on[h]) return;
    HANDLE reader = s.feed.Open() ? s.feed.ReaderHandle() : nullptr;
    HelperValues values;
    values.feed = std::to_wstring((intptr_t)reader);
    auto process = std::make_unique<HelperProcess>();
    std::wstring error = L"cordinal couldn't share what is playing";
    bool started = reader && process->Start(helper.dir + L"/" + visualizer.command.program,
        HelperArguments(visualizer.command, values), helper.dir, error, reader, true);
    if (reader) close((int)(intptr_t)reader);
    if (!started) {
        if (s.shown.empty()) s.feed.Close();
        ShowMessage(L"Couldn't start " + visualizer.label + L": " + error + L".");
        return;
    }
    s.shown.push_back({h, v, std::move(process)});
    ShowMessage(L"Started " + visualizer.label + L".");
}

void OpenHelperVisualizers(float x, float y) {
    auto labels = HelperVisualizerLabels();
    std::vector<std::string> choices;
    for (size_t i = 0; i < labels.size(); ++i)
        choices.push_back((HelperVisualizerRunning(i) ? "Close " : "Start ") + ToUtf8(labels[i]));
    if (choices.empty()) { ShowMessage(L"No enabled helper offers a visualizer."); return; }
    OpenChoice(x, y, choices, -1, [](int index) { ToggleHelperVisualizer(index); });
}

void TickHelperVisualizers() {
    ReapHelperPrograms();
    if (s.shown.empty()) return;
    uint64_t now = SDL_GetTicks();
    if (now - s.feedAt < 16) return;
    s.feedAt = now;
    std::erase_if(s.shown, [&](auto& shown) {
        auto complaint = shown.process->Complaint();  // drains bounded stderr even while the child runs
        if (shown.process->Running()) return false;
        const auto& label = s.helpers[shown.helper]->visualizers[shown.index].label;
        ShowMessage(complaint.empty() ? label + L" closed." : label + L" closed: " + complaint + L".");
        return true;
    });
    if (s.shown.empty()) { s.feed.Close(); return; }
    const bool playing = g.playing != kNone || RadioPlaying(), paused = playing && IsPaused();
    s.feed.Publish([&](HelperFeed& feed) {
        feed.state = !playing ? 0 : paused ? 2 : 1;
        feed.rate = g.engine.Rate();
        feed.position = playing ? g.engine.Position() : 0;
        feed.duration = playing ? g.engine.Duration() : 0;
        const auto& p = CurrentPalette();
        const uint32_t colors[] = {p.bg, p.text, p.sub, p.accent};
        std::copy(std::begin(colors), std::end(colors), feed.colors);
        if (paused) return;
        double clock = -1;
        if (playing && g.engine.VisualSamples(feed.wave, HelperFeed::kFrames, clock) == HelperFeed::kFrames) {
            feed.clock = clock;
            SpectrumLevels(feed.wave, feed.rate, feed.levels, HelperFeed::kBands);
        } else {
            feed.clock = -1;
            std::fill(std::begin(feed.wave), std::end(feed.wave), 0.0f);
            std::fill(std::begin(feed.levels), std::end(feed.levels), 0.0f);
        }
    });
}

int RunningHelperTool() { return s.running.empty() ? -1 : s.runningTool; }

void CancelHelperTool() { s.cancel = true; }

namespace {

void ToolWorker(ToolJob job) {
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
    }
    if (!PostMessageW(nullptr, WM_APP_HELPER, 1, (LPARAM)done)) delete done;
}

}  // namespace

void RunHelperTool(size_t k, const std::vector<TrackId>& ids) {
    if (k >= s.tools.size()) return;
    if (!s.running.empty()) {
        ShowMessage(s.running + L" is still running. Wait for it, or cancel it in the Edit menu.");
        return;
    }
    const Helper& h = *s.helpers[s.tools[k].first];
    const HelperTool& t = h.tools[s.tools[k].second];
    if ((t.perTrack || t.together) && ids.empty()) {
        ShowMessage(L"Select the tracks to run " + t.label + L" on.");
        return;
    }
    ToolJob job;
    job.label = t.label;
    job.program = h.dir + L"/" + t.command.program;
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
            v.format = [id](const std::wstring& pattern) { return FormatForTrack(pattern, id, false); };
            add(HelperArguments(t.command, v), id, FileNameOf(v.file));
        }
    } else {
        add(HelperArguments(t.command, {}), kNone, L"");
    }
    if (s.worker.joinable()) s.worker.join();
    s.cancel = false;
    s.running = t.label;
    s.runningTool = (int)k;
    s.worker = std::thread(ToolWorker, std::move(job));
    ShowMessage(t.label + L" is running...");
}

namespace {

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

void Notice(const std::wstring& title, const std::wstring& text, SDL_MessageBoxFlags flags = SDL_MESSAGEBOX_INFORMATION) {
    SDL_ShowSimpleMessageBox(flags, ToUtf8(title).c_str(), ToUtf8(text).c_str(), g.window);
}

// A tool's tags are a proposal: the user sees them before the tag writer (atomic and undoable) changes any file.
void ProposeTags(ToolDone& d) {
    if (d.edits.empty()) {
        std::wstring failures = Failures(d);
        if (failures.empty()) ShowMessage(d.label + L" proposed no tag changes.");
        else Notice(d.label, d.label + L" proposed no tag changes.\n\n" + failures);
        return;
    }
    std::wstring text = d.label + L" proposes tag changes for " + std::to_wstring(d.edits.size()) + (d.edits.size() == 1 ? L" track:\n" : L" tracks:\n");
    for (size_t i = 0; i < d.edits.size() && i < 8; ++i)
        text += L"\n" + FileNameOf(AudioFileOf(g_lib.Path(d.edits[i].first))) + L": " + Describe(d.edits[i].second);
    if (d.edits.size() > 8) text += L"\nand " + std::to_wstring(d.edits.size() - 8) + L" more";
    if (!d.ignored.empty())
        text += L"\n\nIt also printed " + std::to_wstring(d.ignored.size()) + (d.ignored.size() == 1 ? L" line" : L" lines") +
                L" that cordinal doesn't take as tags, such as: " + d.ignored[0];
    if (!d.failures.empty()) text += L"\n\n" + Failures(d);
    text += L"\n\nWrite these tags? Edit > Undo last tag edit puts the old ones back.";
    if (Confirm(ToUtf8(text), "Proposed tags")) SubmitTagWrites(d.edits, true);
}

// Short output in a message box; longer output in a text file in <data>/reports, opened with the desktop's text viewer.
void ShowOutput(const ToolDone& d) {
    std::wstring text = d.text, failures = Failures(d);
    if (!failures.empty()) text += (text.empty() ? L"" : L"\n\n") + failures;
    if (text.empty()) {
        ShowMessage(d.label + L" finished and printed nothing.");
        return;
    }
    if (std::count(text.begin(), text.end(), L'\n') < 20 && text.size() <= 2000) {
        Notice(d.label, text, d.failures.empty() ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_WARNING);
        return;
    }
    std::error_code ec;
    const fs::path dir = fs::path(compat::Narrow(g.dataDir.c_str())) / "reports";
    fs::create_directories(dir, ec);
    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    char stamp[40];
    strftime(stamp, sizeof(stamp), " %Y-%m-%d %H.%M.%S.txt", &local);
    std::string name = ToUtf8(d.label);
    for (char& c : name)
        if (strchr("/\\:*?\"<>|", c)) c = '_';
    const fs::path file = dir / (name + stamp);
    std::ofstream out(file, std::ios::binary);
    out << ToUtf8(text);
    out.close();
    if (!out) {
        ShowMessage(L"Couldn't save the output of " + d.label + L" in " + FromUtf8(dir.string()) + L".");
        return;
    }
    SDL_OpenURL(("file://" + file.string()).c_str());
    ShowMessage(d.label + L" finished. Its output is in " + FromUtf8(file.string()) + L".");
}

}  // namespace

void OnHelperMessage(WPARAM wp, LPARAM lp) {
    if (wp != 1) return;
    std::unique_ptr<ToolDone> done(reinterpret_cast<ToolDone*>(lp));
    if (s.worker.joinable()) s.worker.join();
    s.running.clear();
    s.runningTool = -1;
    if (done->cancelled) {
        ShowMessage(done->label + L" was cancelled.");
        return;
    }
    if (done->output == HelperTool::Tags) ProposeTags(*done);
    else if (done->output == HelperTool::Text) ShowOutput(*done);
    else if (done->failures.empty()) ShowMessage(done->label + L" finished.");
    else Notice(done->label, Failures(*done), SDL_MESSAGEBOX_WARNING);
}

void StopHelpers() {
    CloseHelperVisualizers();
    s.cancel = true;
    StopHelperPrograms();
    if (s.worker.joinable()) s.worker.join();
}

// Uses only a generated helper in an isolated profile; no installed program or user trust choice changes.
int CheckHelperVisualizerLifecycle() {
    if (ProcessElevated() || access("/usr/bin/python3", X_OK) != 0) {
        printf("skip helper visualizer UI lifecycle: needs a non-root session and the Python fixture interpreter\n");
        return 0;
    }
    if (!s.shown.empty() || s.worker.joinable()) {
        printf("skip helper visualizer UI lifecycle: a user helper is already running\n");
        return 0;
    }
    char temporary[] = "/tmp/cordinal-helper-visualizer-XXXXXX";
    if (!mkdtemp(temporary)) return 1;
    fs::path root(temporary), dir = root / "helpers/feed";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto write = [](const fs::path& file, const std::string& bytes) {
        std::ofstream out(file); out << bytes; out.close(); return !out.fail();
    };
    bool ready = !ec && write(dir / "helper.ini",
        "[helper]\nname=Feed fixture\n[visualizer]\nlabel=Feed fixture\ncommand=reader.py {feed}\n") &&
        write(dir / "reader.py", R"PY(#!/usr/bin/python3
import mmap, pathlib, struct, sys, time
fd = int(sys.argv[1])
block = mmap.mmap(fd, 16616, access=mmap.ACCESS_READ)
assert block[:4] == b'CDVF'
pathlib.Path('ready').write_text('ready')
while True:
    time.sleep(0.02)
)PY");
    fs::permissions(dir / "reader.py", fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec, ec);
    auto helpers = std::move(s.helpers);
    auto on = std::move(s.on);
    auto why = std::move(s.why);
    auto off = std::move(s.off);
    const Settings settings = g.settings;
    const auto dataDir = g.dataDir, settingsPath = g.settingsPath;
    g.dataDir = FromUtf8(root.string());
    g.settingsPath = g.dataDir + L"/settings.ini";
    g.settings.helpersOn = "feed";
    g.settings.helperTrust.clear();
    g.settings.helperTrust["feed"] = ToUtf8(MakeHelperTrust(FromUtf8(dir.string())));
    LoadHelpers();
    bool discovered = ready && HelperVisualizerLabels().size() == 1 && !HelperVisualizerRunning(0);
    if (discovered) ToggleHelperVisualizer(0);
    uint64_t deadline = SDL_GetTicks() + 3000;
    while (HelperVisualizersRunning() && !fs::exists(dir / "ready", ec) && SDL_GetTicks() < deadline) {
        TickHelperVisualizers();
        SDL_Delay(10);
    }
    bool running = HelperVisualizerRunning(0) && fs::exists(dir / "ready", ec);
    if (running) ToggleHelperVisualizer(0);
    bool closed = !HelperVisualizersRunning() && !s.feed.IsOpen();
    if (discovered) ToggleHelperVisualizer(0);
    if (HelperVisualizersRunning()) ToggleHelper(0);  // disabling a trusted fixture needs no confirmation
    bool disabled = !HelperVisualizersRunning() && !s.feed.IsOpen() && !HelperAt(0).on;
    CloseHelperVisualizers();
    s.helpers = std::move(helpers);
    s.on = std::move(on);
    s.why = std::move(why);
    s.off = std::move(off);
    g.settings = settings;
    g.dataDir = dataDir;
    g.settingsPath = settingsPath;
    ApplyHelpers();
    fs::remove_all(root, ec);
    bool passed = discovered && running && closed && disabled && !ec;
    printf("%s enabled helper visualizers start, close, release their feed and stop when their helper is disabled\n",
        passed ? "ok  " : "FAIL");
    return !passed;
}

}  // namespace lx
