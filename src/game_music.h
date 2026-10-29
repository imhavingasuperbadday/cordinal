// Video game music through the emulators in chiptune/: SPC (SNES), NSF/NSFE (NES), GBS (Game
// Boy), VGM/VGZ (Master System, Genesis), GYM and SID (Commodore 64). One file can hold several songs; song N of
// "file.nsf" is addressed as "file.nsf|N" (1-based), the same subtrack scheme as CUE sheets.
#pragma once
#include "decoder.h"
#include "tags.h"

#include <memory>
#include <string>

// True for a game music file by extension, plain or with a "|N" song suffix.
bool IsGameMusicFile(const std::wstring& path);
// Splits "file.nsf|3" into the file and song 3. A plain path is song 1. False if the suffix is malformed.
bool SplitGameMusicPath(const std::wstring& path, std::wstring& file, int& subsong);

// Number of songs in the file, 0 if it can't be read.
int GameMusicTrackCount(const std::wstring& file);

// Decoder for one song (1-based), interleaved stereo float at 44100 Hz. The song stops at its
// play length (2.5 minutes when the rip doesn't say) and fades out over its last 8 seconds.
// Emulators only run forward, so seeking backwards restarts the song.
std::unique_ptr<Decoder> OpenGameMusicDecoder(const std::wstring& file, int subsong, std::wstring& error);

// Title, album (game), artist, date (from the copyright), comment (system and dumper), track
// numbers, duration. False if the file can't be read or has no such song.
bool ReadGameMusicTags(const std::wstring& file, int subsong, TagData& out);
