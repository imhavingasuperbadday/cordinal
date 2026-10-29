// Independent FFmpeg oracle for fixture validation; test executable only.
// Wraps an out-of-process ffmpeg (decoding) and ffprobe, and parses MP4 gapless metadata directly,
// so the tag-writer and self tests can cross-check the player against a reference implementation
// that is never compiled or linked into the shipping player.
#pragma once
#include "decoder.h"
#include "command_line.h"
#include <windows.h>

#include <string>

struct FFmpegTools {
    std::wstring ffmpeg, ffprobe;
};

// Looked up once: CORDINAL_FFMPEG, next to cordinal.exe, PATH, Program Files, WinGet (on Linux:
// CORDINAL_FFMPEG, then PATH).
const FFmpegTools* FindFFmpeg();

std::unique_ptr<Decoder> OpenFFmpegDecoder(const std::wstring& path, unsigned rate);

// MP4 metadata checks for the tag writer, including formats the player cannot decode.
struct GaplessInfo { long long skip = 0, valid = 0; };
bool ReadMp4Gapless(const std::wstring& path, GaplessInfo& info);

// A child process whose stdout is read through a pipe. It never shows a console and is
// killed automatically if cordinal exits.
class ChildProcess {
public:
    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ~ChildProcess();

    bool Start(const std::wstring& exe, const std::wstring& args);
    size_t Read(void* buffer, size_t bytes);     // 0 at EOF
    std::string ReadAll();
    void Kill();  // safe to call from another thread while Read blocks

private:
    HANDLE process_ = nullptr;  // on Linux: the process id
    HANDLE out_ = nullptr;      // on Linux: the pipe's file descriptor + 1
};
