// CUE sheets: several songs inside one audio file (a FLAC/APE/WAV with a .cue next to it, or a
// FLAC with an embedded CUESHEET). Each song is a track whose path is "file|N" (SplitSubtrack),
// N being the TRACK number. Sheets are read from disk; parsed ones are cached.
#pragma once
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Decoder;

struct CueTrack {
    int number = 0;
    std::string title, performer, songwriter, isrc;  // UTF-8
    double start = 0;  // INDEX 01, seconds
    double end = 0;    // where the next track's INDEX 01 starts; 0 = to the end of the file
};

struct CueSheet {
    std::string title, performer, date, genre, comment, catalog;  // UTF-8
    int discNumber = 0, totalDiscs = 0;
    struct File {
        std::wstring path;  // the audio file, resolved against the cue's folder
        std::vector<CueTrack> tracks;
    };
    std::vector<File> files;
    size_t TrackCount() const {
        size_t n = 0;
        for (const File& f : files) n += f.tracks.size();
        return n;
    }
};

// Parses cue text (UTF-8). FILE names are relative to `cueDir`; a name whose file doesn't exist is
// replaced by a same-stem audio file next to it ("Album.wav" in the cue, "Album.flac" on disk).
// False if there's no usable track or the text is over 1 MB.
bool ParseCue(std::string_view text, const std::wstring& cueDir, CueSheet& out);
// Reads a .cue in any common encoding (UTF-8 with or without BOM, UTF-16 LE/BE with BOM, or the
// ANSI code page) and parses it.
bool LoadCueFile(const std::wstring& cuePath, CueSheet& out);
// The sheet embedded in a FLAC: its CUESHEET Vorbis comment, else the binary CUESHEET metadata
// block. The sheet's file is `audioPath`.
bool ReadEmbeddedCue(const std::wstring& audioPath, CueSheet& out);

// "file.flac|3" -> the track numbered 3 of the sheet covering file.flac.
struct CueSubtrack {
    std::shared_ptr<const CueSheet> sheet;
    size_t file = 0;   // index into sheet->files
    size_t track = 0;  // index into sheet->files[file].tracks
    std::wstring audioPath;
    uint64_t cueTime = 0;  // write time of the cue (0 for an embedded sheet)
    const CueTrack& Track() const { return sheet->files[file].tracks[track]; }
};
// False if the path isn't a sub-track or no current sheet has that track.
bool LookupSubtrack(const std::wstring& path, CueSubtrack& out);
// The track's [start, end) in seconds of its file (end 0 = to the end). False if there's no such track.
bool SubtrackRange(const std::wstring& path, double& start, double& end);
// Opens a track for decoding: a sub-track is limited to its range. Null, with `error` set, when
// its sheet can't be found any more.
std::unique_ptr<Decoder> OpenTrackDecoder(const std::wstring& path, unsigned fallbackRate, std::wstring& error);
// The real file behind a path: "file.flac|3" -> "file.flac", a plain path unchanged.
std::wstring AudioFileOf(const std::wstring& path);
// True when the path's extension is .cue.
bool IsCueFile(const std::wstring& path);

// The sheets of one folder: a .cue file covers the audio files it names. Build one per folder
// when expanding many files; construction lists the folder and parses its cue files (cached).
class CueFolder {
public:
    explicit CueFolder(const std::wstring& dir);
    // The sub-tracks ("audioPath|N") of an audio file in this folder: those of a covering .cue,
    // else of its embedded sheet, else the game-music hook. Empty for a plain file. `cueTime`
    // (optional) receives the write time of the covering .cue, 0 if none.
    std::vector<std::wstring> Expand(const std::wstring& audioPath, uint64_t* cueTime = nullptr) const;

    struct Cover {
        std::shared_ptr<const CueSheet> sheet;
        size_t file = 0;
        uint64_t time = 0;
    };
    // The sheet that names this audio file, if a .cue in this folder does.
    bool Covering(const std::wstring& audioPath, Cover& out) const;

private:
    std::vector<std::pair<std::wstring, Cover>> covers_;  // lower-case audio path -> cover
};

// Every track of a .cue (its files must exist) as sub-track paths.
bool ExpandCueFile(const std::wstring& cuePath, std::vector<std::wstring>& out);
// Sub-track paths for an audio file (see CueFolder::Expand); empty for a plain file.
std::vector<std::wstring> ExpandSubtracks(const std::wstring& audioPath);

// Multi-song game music files (NSF, GBS, SID, ...) expand into "file|1".."file|N": the number of
// songs, or 0 when the file is a plain track (one song, or not game music).
int GameMusicSubtrackCount(const std::wstring& audioPath);
