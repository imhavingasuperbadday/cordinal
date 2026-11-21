// Library > Convert...: converting songs to .cord, FLAC, WAV, MP3 or AAC, either as copies (beside the
// originals or in another folder) or replacing the originals in the library. Every new file is
// written beside its destination, read back in full and checked (lossless ones sample for sample)
// before it appears, and only then may an original go to the Recycle Bin. Replacements write a log,
// so they can be undone while the log lasts (docs/cord-format.md, "Converting inside cordinal").
// The library itself isn't touched here; callers move their tracks with the results.
#pragma once
#include "library.h"
#include "tags.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

class PcmReader;

// The formats Convert writes. None is cordinal's own .cord (named so for the settings and menus
// that predate the other formats).
enum class CordExport : uint8_t { None, Flac, Wav, Mp3_192, Mp3_320, Aac128, Aac192, Count };
struct CordExportInfo { const wchar_t *label, *extension; unsigned kbps; };
inline constexpr CordExportInfo kCordExports[] = {
    {L".cord (lossless, cordinal's own)", L"cord", 0}, {L"FLAC (lossless)", L"flac", 0}, {L"WAV (lossless)", L"wav", 0},
    {L"MP3 192 kb/s (lossy)", L"mp3", 192}, {L"MP3 320 kb/s (lossy)", L"mp3", 320},
    {L"AAC 128 kb/s (.m4a, lossy)", L"m4a", 128}, {L"AAC 192 kb/s (.m4a, lossy)", L"m4a", 192},
};

// Copy leaves the original and the library alone. Replace switches the library to the new file and
// sends the original to the Recycle Bin (or keeps it, by preference), with undo. Auto is how the
// menus worked before the Convert window: .cord replaces, every other format copies.
enum class ConvertMode : uint8_t { Auto, Replace, Copy };

struct ConvertItem {
    TrackId id = 0;
    std::wstring from, to;
    std::wstring skip;  // why it won't be converted (empty = it will)
    CordExport format = CordExport::None;
    bool replace = false;  // a replacement: logged for undo, the original recycled or kept
};

// Where each file would go, or why it can't. A replacement, or a copy with no `folder`, goes beside
// its original under the same name; a copy into `folder` keeps the folders below the selection's
// common folder, so albums stay apart (one folder's songs keep that folder's name). Never a part of
// a cue sheet, never over an existing file or another planned output, and never a file already in
// the target format. Lossless targets (.cord, FLAC, WAV) need a source whose exact samples cordinal
// reads (ReadsExactly); MP3 and AAC take anything cordinal plays. `codec`, when given, says what the
// library knows of a track, which tells ALAC from AAC in an .m4a without opening it.
std::vector<ConvertItem> PlanConvert(const std::vector<std::pair<TrackId, std::wstring>>& tracks,
                                     CordExport format = CordExport::None, ConvertMode mode = ConvertMode::Auto,
                                     const std::wstring& folder = {}, const std::function<Codec(TrackId)>& codec = {});

// True when cordinal can read this file's exact samples (OpenPcmReader), judged by its extension and,
// for MP4 and CAF, its codec when known (Unknown: perhaps).
bool ReadsExactly(const std::wstring& path, Codec codec = Codec::Unknown);

// Copies the source's exact samples, tags and pictures into a FLAC file. The FLAC is verified in a
// sibling temporary file before it is published; cancellation or failure leaves both source and any
// existing destination untouched. No library or conversion log is needed.
bool ExportFlac(const std::wstring& from, const std::wstring& to, std::wstring& error, const std::atomic<bool>& cancel);
// Writes `from` as `format` at `to`, which must not exist yet. .cord, FLAC and WAV compare exact PCM;
// lossy copies check source integrity, complete decoding, length and metadata. MP3 and AAC use
// built-in codecs and create 48 kHz stereo copies (MP3 with a LAME header, so they play gaplessly).
// `threads` is the .cord encoder's.
bool ExportAudio(const std::wstring& from, const std::wstring& to, CordExport format, std::wstring& error,
                 const std::atomic<bool>& cancel, unsigned threads = 1);

// Writes `source`, read once to its end, as FLAC (`flac`) or .cord at `to`, which must not exist
// yet, with `meta` (.cord META entries) and `pictures` (PICT payloads) as its tags. Verified like
// ExportAudio's lossless copies before it appears; on failure nothing is left behind. CD rips use it.
bool WriteLossless(PcmReader& source, const std::string& meta, const std::vector<std::string>& pictures, const std::wstring& to,
                   bool flac, std::wstring& error, const std::atomic<bool>& cancel, unsigned threads = 1);

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
    std::wstring logError;  // why originals were kept: the undo log couldn't be written
    bool cancelled = false;
};

struct ConvertSettings {
    bool recycle = true;  // send verified originals to the Recycle Bin
    unsigned threads = 1;
};

// Converts the planned files in order on the calling thread, reporting each file's index before it
// starts, and creating a copy's folders as needed. A file whose conversion can't be verified leaves
// nothing behind and its original as it was. Stops between files when `cancel` is set.
ConvertResult ExecuteConvert(const std::vector<ConvertItem>& plan, const ConvertSettings& settings, const std::wstring& logDir,
                             const std::atomic<bool>& cancel, const std::function<void(size_t)>& progress);

struct ConvertUndone {
    std::wstring cord, original;  // `cord` is the converted file, whatever its format
    bool removed = false;  // the converted file went to the Recycle Bin
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

// The Convert window's line under its options, the same on Windows and Linux: how many songs a plan
// converts, and why the rest won't (the commonest reasons first).
std::wstring ConvertSummary(const std::vector<ConvertItem>& plan);
// Asked before replacing originals: what happens to them, how long undo lasts, and a warning when
// lossless originals would be replaced by a lossy format.
std::wstring ReplaceQuestion(const std::vector<ConvertItem>& plan, bool recycle, bool showOriginals, unsigned days);
// The status line once a run ends.
std::wstring ConvertDoneText(const ConvertResult& result, CordExport format, bool replaced, bool recycle);
