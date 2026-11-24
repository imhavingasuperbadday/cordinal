// Library linter (Library > Lint library...): catches common metadata and cover-art messes
// across the library — split albums, near-duplicate artists, mojibake / legacy encoding in tags,
// and cover-art issues — previews every change, and builds atomic TagEdits so checked fixes
// apply through SubmitTagWrites with full multi-file undo (Edit > Undo last tag edit).
#pragma once
#include "library.h"
#include "tagwriter.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lint {

enum class LintKind : uint8_t {
    SplitAlbum,
    NearDuplicateArtist,
    Mojibake,
    CoverArt,
};

enum class CoverIssue : uint8_t {
    None,
    BloatedPng,
    Inconsistent,
    LowRes,
    NonSquare,
};

struct LintFix {
    TrackId track = kInvalidTrack;
    TagEdit edit;
    bool applyReplacementCover = false;
};

struct LintFinding {
    LintKind kind = LintKind::SplitAlbum;
    CoverIssue coverIssue = CoverIssue::None;
    bool checked = true;
    bool fixable = true;
    std::wstring summary;
    std::wstring before;
    std::wstring after;
    std::string replacementCover;  // shared cover bytes for CoverIssue::BloatedPng / Inconsistent
    std::vector<LintFix> fixes;
};

// Re-encodes a bloated PNG or oversized image to a compact JPEG (<= 1500 px).
using CoverReencoder = std::function<bool(const std::string& input, std::string& jpegOut)>;

// Fast in-memory lints (no disk I/O): split albums, near-duplicate artists, mojibake.
void FindMetadataIssues(const Library& lib, std::vector<LintFinding>& out);

// Lightweight track snapshot for background cover-art auditing without touching Library on a worker.
struct CoverTrackInfo {
    TrackId id = kInvalidTrack;
    uint32_t dir = 0;
    std::string albumFold;
    std::wstring album;
    std::wstring path;
    bool writable = false;
};

std::vector<CoverTrackInfo> SnapshotCoverTracks(const Library& lib);
void AuditCoverTracks(const std::vector<CoverTrackInfo>& tracks, const CoverReencoder& reencode,
                      std::vector<LintFinding>& out, const std::atomic<bool>* cancel = nullptr,
                      const std::function<void(size_t done, size_t total)>& progress = {});

// Disk-backed cover-art audit: bloated PNGs, inconsistent album covers, low-res (<500 px),
// and non-square covers. Safe to run on a worker thread via SnapshotCoverTracks + AuditCoverTracks.
void AuditCoverArt(const Library& lib, const CoverReencoder& reencode, std::vector<LintFinding>& out,
                   const std::atomic<bool>* cancel = nullptr,
                   const std::function<void(size_t done, size_t total)>& progress = {});

// Runs all four lints in sequence.
std::vector<LintFinding> LintLibrary(const Library& lib, const CoverReencoder& reencode = {},
                                     const std::atomic<bool>* cancel = nullptr);

// Merges all checked, fixable findings into one TagEdit per TrackId for SubmitTagWrites.
std::vector<std::pair<TrackId, TagEdit>> CollectCheckedEdits(const std::vector<LintFinding>& findings);

// Exposed for unit tests: attempts to repair Shift-JIS, CP1251, or UTF-8-as-Latin-1 mojibake.
bool DetectMojibake(std::string_view utf8, std::string& repairedUtf8, std::wstring& encodingName);

// Exposed for unit tests: canonicalizes an artist name for near-duplicate clustering.
std::string CanonicalArtistKey(std::string_view utf8);

}  // namespace lint
