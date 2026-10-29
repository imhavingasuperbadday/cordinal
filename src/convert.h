// Converting lossless files to cordinal's own format, and exporting .cord to standard FLAC.
// For .cord conversion (docs/cord-format.md, "Converting inside cordinal"), each file is encoded
// next to itself, read back in full and compared with its source
// sample for sample before anything else happens, and only then is its original sent to the
// Recycle Bin or kept. Every run writes a log, so conversions can be undone while the log lasts.
// The library itself isn't touched here; callers move their tracks with the results.
#pragma once
#include "library.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

// None is the original conversion into .cord; the other choices create independent exports.
enum class CordExport : uint8_t { None, Flac, Wav, Mp3_192, Mp3_320, Aac128, Aac192, Count };
struct CordExportInfo { const wchar_t *label, *extension; unsigned kbps; };
inline constexpr CordExportInfo kCordExports[] = {
    {L".cord", L"cord", 0}, {L"FLAC (lossless)", L"flac", 0}, {L"WAV (lossless)", L"wav", 0},
    {L"MP3 192 kb/s (lossy)", L"mp3", 192}, {L"MP3 320 kb/s (lossy)", L"mp3", 320},
    {L"AAC 128 kb/s (.m4a, lossy)", L"m4a", 128}, {L"AAC 192 kb/s (.m4a, lossy)", L"m4a", 192},
};

struct ConvertItem {
    TrackId id = 0;
    std::wstring from, to;
    std::wstring skip;  // why it won't be converted (empty = it will)
    CordExport format = CordExport::None;  // exports keep the source and leave conversion logs alone
};

// Where each file would go (same folder and name, ".cord"), or why it can't: only files whose
// exact samples cordinal can read (FLAC, integer WAV and AIFF) convert, never a part of a cue
// sheet, and never over an existing file.
// Exports accept whole .cord files and write the selected format beside them.
std::vector<ConvertItem> PlanConvert(const std::vector<std::pair<TrackId, std::wstring>>& tracks, CordExport format = CordExport::None);

// Reads only the .cord file, preserving its exact samples, tags and pictures. The FLAC is verified
// in a sibling temporary file before it is published; cancellation or failure leaves both source
// and any existing destination untouched. No original file, library or conversion log is needed.
bool ExportCordFlac(const std::wstring& from, const std::wstring& to, std::wstring& error, const std::atomic<bool>& cancel);
// FLAC/WAV compare exact PCM; lossy copies check source integrity, complete decoding, length and
// metadata. MP3 and AAC use built-in codecs and create 48 kHz stereo copies.
bool ExportCordAudio(const std::wstring& from, const std::wstring& to, CordExport format, std::wstring& error,
                     const std::atomic<bool>& cancel);

// The source's tags as the new file's META entries and PICT payloads. FLAC's Vorbis comments and
// pictures are carried over whole: the fields cordinal shows become typed entries, everything else
// (MusicBrainz ids, chapters, loop points...) free-form ones under its own key. Other sources give
// the fields cordinal reads.
std::string CordTagsFrom(const std::wstring& source, std::vector<std::string>& pictures);

struct ConvertDone {
    TrackId id = 0;
    std::wstring from, to;
    bool recycled = false;  // the original went to the Recycle Bin; otherwise it was kept
};

struct ConvertResult {
    std::vector<ConvertDone> done;
    std::vector<std::pair<std::wstring, std::wstring>> failed;  // file -> what went wrong
    std::wstring log;     // the log written, or ""
    bool cancelled = false;
};

struct ConvertSettings {
    bool recycle = true;  // send verified originals to the Recycle Bin
    unsigned threads = 1;
};

// Converts the planned files in order on the calling thread, reporting each file's index before it
// starts. A file whose conversion can't be verified leaves no .cord behind and its original as it
// was. Stops between files when `cancel` is set.
ConvertResult ExecuteConvert(const std::vector<ConvertItem>& plan, const ConvertSettings& settings, const std::wstring& logDir,
                             const std::atomic<bool>& cancel, const std::function<void(size_t)>& progress);

struct ConvertUndone {
    std::wstring cord, original;
    bool removed = false;  // the .cord went to the Recycle Bin
};

// Reverses the newest conversion log in `logDir`: originals come back (from the Recycle Bin when
// they went there), and each .cord whose original is back goes to the Recycle Bin when `recycle`
// is set. A file whose original can't be brought back keeps its .cord, which holds the same audio,
// and is reported in `error`. The log is removed once nothing in it is left to undo.
std::vector<ConvertUndone> UndoLastConvert(const std::wstring& logDir, bool recycle, std::wstring& error);

// True when `logDir` holds a conversion that can still be undone.
bool CanUndoConvert(const std::wstring& logDir);

// Forgets logs older than `days`: those conversions can no longer be undone.
void PruneConvertLogs(const std::wstring& logDir, unsigned days);

// Sends a file to the Recycle Bin, never deleting it outright: false (with the file untouched)
// when its drive has no Recycle Bin or the shell refuses.
bool RecycleFile(const std::wstring& path);
// Brings a file back from the Recycle Bin to `original`, its old place. False if it isn't there.
bool RestoreFromRecycleBin(const std::wstring& original);
