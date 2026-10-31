// Writing tags back into audio files: MP3 (ID3v2), FLAC, Ogg Vorbis / Opus (Vorbis comments),
// MP4 / M4A (iTunes ilst atoms) and cordinal's own format (its META and PICT chunks, cord.h).
//
// Only the fields set in a TagEdit change; everything else in the file (other frames and
// atoms, pictures unless replaced, audio data) is kept byte for byte. A failed write leaves
// the file as it was: when the tag can't be rewritten in its padding, the new file is built
// next to the original and swapped in with ReplaceFile.
#pragma once
#include "streamfile.h"
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

struct TagEdit {
    // Text fields, UTF-8. nullopt = leave unchanged, "" = remove the field.
    std::optional<std::string> title, artist, album, albumArtist, genre, date, composer, comment, grouping, label, lyrics;
    // Numbers. nullopt = leave unchanged, 0 = remove.
    std::optional<uint32_t> track, trackTotal, disc, discTotal, bpm;
    std::optional<bool> compilation;  // false removes the flag
    // Front cover as encoded image bytes (JPEG or PNG). "" removes every embedded picture.
    std::optional<std::string> cover;
    // ReplayGain tags as text (e.g. "+1.25 dB", "0.988000"). nullopt = leave unchanged,
    // "" = remove. The R128 fields are Opus's native Q7.8 dB tags relative to -23 LUFS.
    std::optional<std::string> rgTrackGain, rgTrackPeak, rgAlbumGain, rgAlbumPeak;
    std::optional<std::string> r128TrackGain, r128AlbumGain;
    // Listening statistics in the freedesktop FMPS form other players read: a rating from "0.0" to
    // "1.0" and a play count in digits. "" removes. MP3 also gets a POPM frame of cordinal's own.
    std::optional<std::string> rating, playCount;
    // True when no field is set, i.e. this edit would change nothing (WriteTags then leaves the file alone).
    bool Empty() const;
};

// Formats WriteTags supports, by extension: mp3, flac, ogg/oga, opus, m4a/m4b/mp4, cord. A CUE
// subtrack or a song inside a ZIP archive has no file of its own to rewrite, so it is false too.
bool CanWriteTags(const std::wstring& path);
bool WriteTags(const std::wstring& path, const TagEdit& edit, std::wstring& error);

// ---------------------------------------------------------------------------
// Shared by the per-format writers (tagwriter_*.cpp).

bool ReadFileBytes(const std::wstring& path, std::string& out);

// Replaces the file's whole content: writes a temp file in the same folder, then ReplaceFile
// (keeps the original's attributes, ACLs and creation time). The original is untouched on failure.
bool ReplaceFileBytes(const std::wstring& path, const std::string& data, std::wstring& error);

// Per-format writers. Each returns false with `error` set and the file untouched on failure.
bool WriteTagsMp3(const std::wstring& path, const TagEdit& edit, std::wstring& error);
bool WriteTagsMp4(const std::wstring& path, const TagEdit& edit, std::wstring& error);
bool WriteTagsFlac(const std::wstring& path, const TagEdit& edit, std::wstring& error);
bool WriteTagsOgg(const std::wstring& path, const TagEdit& edit, std::wstring& error);
bool WriteTagsCord(const std::wstring& path, const TagEdit& edit, std::wstring& error);

// Self-tests for the writers (selftest.cpp calls these). They write "PASS ..."/"FAIL ..." lines
// to `log` like the rest of the self-test, work only on copies inside `scratchDir`, and return
// the number of failures. `fixtures` is the fixtures root (media\, tags2\ ...).
int TestTagWriterMp3Mp4(const std::wstring& fixtures, const std::wstring& scratchDir, FILE* log);
int TestTagWriterFlacOgg(const std::wstring& fixtures, const std::wstring& scratchDir, FILE* log);
// Shared by those (tagwriter_test.cpp): the full edit/read-back/bit-identical-audio matrix on
// each file, FFmpeg for making variants, and cleanup.
int TestTagWriterFiles(const std::vector<std::wstring>& files, FILE* log, const wchar_t* label,
                       bool checkOpusR128 = false);
bool RunFFmpegForTest(const std::wstring& args);
void LogSkipped(FILE* log, const std::wstring& name);
void DeleteTestFolder(const std::wstring& dir);
