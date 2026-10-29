// Helpers: separate programs that do what cordinal doesn't do itself (docs/plugins.md, "Helpers").
// A helper is a folder in <data>\helpers whose helper.ini names its programs and what they provide:
// a decoder for file types cordinal doesn't read, tools that run on selected tracks, and
// visualizers that draw what is playing. Every program runs as a child process inside a job that
// ends it with cordinal, so a helper that crashes or hangs costs one track or one tool run, never
// the player. This file holds the parts that need no window: the manifest, command lines, child
// processes, trust records, the decoder, tag reading and the visual feed. app_helpers.cpp turns
// helpers on and off, runs their tools and starts their visualizers. The visual feed and the visualizers are
// Windows only; on Linux a program is any executable file in the helper's folder, started with posix_spawn.
#pragma once
#include "decoder.h"
#include "tags.h"
#include "tagwriter.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The helper.ini format this cordinal reads. A helper written for a later one is left out with a
// reason, never half-read.
constexpr int kHelperFormat = 1;

// One command line of a manifest: a program in the helper's folder, then its arguments, which may
// hold placeholders ({file}, {files}, {start}, {feed}, or a title format such as {%artist%}).
struct HelperCommand {
    std::wstring program;  // relative to the helper's folder; empty: no command
    std::vector<std::wstring> args;
    bool Has(const wchar_t* placeholder) const;  // a name without braces: "start"
    bool UsesTrack() const;                       // {file} or a title format
};

struct HelperTool {
    enum Output : uint8_t { Text, Tags, Nothing };
    std::wstring label;
    HelperCommand command;
    Output output = Text;
    bool together = false;  // {files}: one run for the whole selection
    bool perTrack = false;  // {file} or a title format: one run per track
};

struct HelperVisualizer {
    std::wstring label;
    HelperCommand command;
};

struct Helper {
    std::wstring id, dir;  // its folder's name and path
    std::wstring name, version, author, description;
    std::wstring error;                    // why none of it can be used
    std::vector<std::wstring> warnings;    // what was left out, in plain words
    std::vector<std::wstring> extensions;  // the file types it plays: lowercase, no dot
    HelperCommand decode, info;
    std::vector<HelperTool> tools;
    std::vector<HelperVisualizer> visualizers;
    std::vector<std::wstring> programs;    // every program its commands start, once each
};

// Reads one helper.ini. Every program a command names must be an .exe inside `dir`; file types
// cordinal plays itself stay cordinal's.
Helper ParseHelper(const std::wstring& id, const std::wstring& dir, const std::string& ini);
// Every helper folder under `root`, in name order.
std::vector<Helper> ReadHelpers(const std::wstring& root);
// What a helper does, in words: "plays .tak and .ofr files and adds the tool Measure loudness".
std::wstring HelperSummary(const Helper& h);
// "ffmpeg.exe and ffprobe.exe"
std::wstring HelperProgramList(const Helper& h);

// Why no helper may run in this process, or empty when they may: safe mode, or a cordinal running
// as administrator, which would hand its rights to programs in a folder anyone logged in as the user
// can change.
std::wstring HelpersOffReason(bool safe, bool elevated);
bool ProcessElevated();

// What a command's placeholders stand for. Title formats go through `format`.
struct HelperValues {
    std::wstring file;                // {file}
    std::vector<std::wstring> files;  // {files}: one argument each
    double start = 0;                 // {start}, in seconds
    std::wstring feed;                // {feed}
    std::function<std::wstring(const std::wstring& pattern)> format;
};
// The command's arguments as one command line, each quoted so that it reaches the program whole,
// whatever a tag or file name holds.
std::wstring HelperArguments(const HelperCommand& c, const HelperValues& v);

// A helper program running as a child process. Its stdin reads nothing; stdout and stderr come back
// through pipes, except for a program shown in a window (a visualizer), which keeps its own.
class HelperProcess {
public:
    HelperProcess() = default;
    HelperProcess(const HelperProcess&) = delete;
    HelperProcess& operator=(const HelperProcess&) = delete;
    ~HelperProcess();
    // `program` is a full path; `inherit` is one more handle the program gets (the visual feed).
    bool Start(const std::wstring& program, const std::wstring& arguments, const std::wstring& dir, std::wstring& error,
               HANDLE inherit = nullptr, bool window = false);
    // Up to `bytes` of its output, waiting until `deadline` (GetTickCount64; 0 for no limit) for the
    // first of them. 0 at the end of its output, after Stop or `cancel`, or on a timeout; the last
    // two also stop it.
    size_t Read(void* buffer, size_t bytes, ULONGLONG deadline, const std::atomic<bool>* cancel = nullptr);
    // All of its output, at most `limit` bytes. Without `cancel` it may stay silent for the patience
    // at most; with it, for as long as it likes until `cancel` is set. False unless it ended by itself.
    bool ReadAll(std::string& out, size_t limit, const std::atomic<bool>* cancel = nullptr);
    // Once its output has ended: its exit code after waiting up to `ms` for it to exit, stopping it
    // if it hasn't.
    DWORD Finish(DWORD ms);
    void Stop();  // ends it now; safe from another thread
    bool Running() const;
    bool TimedOut() const { return timedOut_; }
#ifdef _WIN32
    HANDLE Process() const { return process_; }
#endif
    // The last line it wrote to stderr, for messages.
    std::wstring Complaint();

private:
    void DrainErrors();
#ifdef _WIN32
    HANDLE process_ = nullptr, out_ = nullptr, err_ = nullptr;
#else
    bool Exited() const;
    pid_t pid_ = 0;
    int out_ = -1, err_ = -1;
    mutable bool exited_ = false;
    mutable DWORD status_ = 0;
#endif
    std::string errors_;
    std::atomic<bool> stopped_{false};
    bool timedOut_ = false;
};

// Ends every helper program still running (cordinal is closing).
void StopHelperPrograms();
// How long a helper may stay silent before cordinal gives up on it: 15 seconds. The self-test
// shortens it.
void SetHelperPatience(unsigned milliseconds);
unsigned HelperPatience();

// A record of a helper's files (helper.ini and every .exe and .dll in its folder), made when the
// user turns it on and kept in settings.ini.
std::wstring MakeHelperTrust(const std::wstring& dir);
// True when the files still match `record`. Only files whose size or time changed are hashed again;
// when their contents still match, `record` is brought up to date so the next start skips them.
bool CheckHelperTrust(const std::wstring& dir, std::wstring& record);

// The helpers that are on, as the decoder, the scanner and the tag reader see them on any thread.
// The first in the list wins a file type two of them claim.
void SetActiveHelpers(std::vector<std::shared_ptr<const Helper>> on);
// The helper that plays files of this type (lowercase, no dot), or null.
std::shared_ptr<const Helper> HelperForExtension(const std::wstring& ext);
// ";*.tak;*.ofr" for the open dialog: the file types the helpers that are on play.
std::wstring HelperFileTypes();
// A decoder for a file a helper plays; null with `error` set when it can't start.
std::unique_ptr<Decoder> OpenHelperDecoder(const std::wstring& path, std::wstring& error);
// Tags through the helper's info command. Without one the tags stay empty and the file name stands
// in, which is not a failure.
bool ReadHelperTags(const std::wstring& path, TagData& out);
// What a program printed, as text: UTF-8 when it is valid UTF-8, else the console's code page,
// which is what older Windows tools write.
std::wstring HelperText(const std::string& bytes);
// "field=value" lines, as a helper's info command or a tool prints them, into tags.
void ParseHelperInfo(const std::string& text, TagData& out);
// The same lines as a tag edit for a tool's proposal. `ignored` names the lines it can't use.
void ParseHelperEdit(const std::string& text, TagEdit& edit, std::vector<std::wstring>& ignored);

#ifdef _WIN32
// The visual feed: what is playing, for a visualizer to draw in sync. It is shared memory the
// visualizer maps read-only through the handle {feed} names; cordinal rewrites it about 60 times a
// second while a visualizer runs. `sequence` is odd while cordinal writes: a reader copies the block
// and keeps the copy when the sequence was even and the same before and after (ReadHelperFeed).
// Fields are only ever appended; `size` says how much of it this cordinal writes.
struct HelperFeed {
    static constexpr uint32_t kFrames = 2048, kBands = 40;
    char magic[4];                // "CDVF"
    uint32_t version;             // 1
    uint32_t size;                // sizeof(HelperFeed)
    uint32_t frames, bands;       // kFrames, kBands
    volatile LONG sequence;       // everything after it changes while a visualizer runs
    uint32_t state;               // 0 stopped, 1 playing, 2 paused
    uint32_t rate;                // the output's sample rate
    double clock;                 // output time in seconds at the middle of wave; -1 when nothing plays
    double position, duration;    // in the playing track, seconds
    uint32_t colors[4];           // the theme: background, text, secondary text, accent (0xRRGGBB)
    float levels[kBands];         // spectrum, 0..1 over 72 dB, 40 Hz to 20 kHz on a log scale
    float wave[kFrames * 2];      // interleaved stereo, -1..1, before the volume
};

class HelperFeedWriter {
public:
    ~HelperFeedWriter() { Close(); }
    bool Open();
    // A read-only handle for one visualizer, inheritable; the caller closes it once it started.
    HANDLE ReaderHandle() const;
    // Fills `block` through the callback, between the sequence's odd and even steps.
    void Publish(const std::function<void(HelperFeed& block)>& fill);
    void Close();
    bool IsOpen() const { return view_ != nullptr; }

private:
    HANDLE mapping_ = nullptr;
    HelperFeed* view_ = nullptr;
};
// The reader's side: a consistent copy of the block, false when cordinal kept writing through it.
bool ReadHelperFeed(const HelperFeed* shared, HelperFeed& copy);
#endif
