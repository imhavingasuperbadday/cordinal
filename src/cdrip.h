// Library > Rip CD (docs/library-roadmap.md, step 5): the disc's table of contents and its MusicBrainz
// disc id, names from CD-Text or a MusicBrainz lookup, and a verified rip straight into FLAC or .cord.
// Every sector is read twice in two passes, the second after the drive's cache has been pushed out,
// and a sector whose two reads differ is read again until two reads agree. The drive's read offset
// shifts the audio by whole samples, as AccurateRip's offsets are measured. Tracks are encoded as
// they are read, so a rip costs no temporary WAV and no second disk pass. The drive itself is a small
// interface: Windows (IOCTL_CDROM_RAW_READ) and Linux (CDROMREADAUDIO) each give one, and the
// self-test gives a disc in memory with sectors that misread.
#pragma once
#include "album_tagger.h"
#include "decoder.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cdrip {

constexpr unsigned kSectorBytes = 2352, kSectorFrames = 588, kPregap = 150;
// Offsets a drive can need: AccurateRip's list runs from about -1,200 to +2,000 samples.
constexpr int kMaxOffset = 3000;

struct Toc {
    struct Track {
        uint8_t number = 0;
        uint32_t start = 0;  // LBA
        bool audio = true;
    };
    std::vector<Track> tracks;  // every track, data ones too, in order
    uint32_t leadout = 0;       // LBA
    // The sector after track i's audio. A data track in a second session (Enhanced CD) begins
    // 11,400 sectors after the audio session's lead-out.
    uint32_t End(size_t i) const;
    // The audio tracks' indices into `tracks`.
    std::vector<size_t> Audio() const;
};

class Drive {
public:
    virtual ~Drive() = default;
    virtual bool ReadToc(Toc& toc) = 0;
    // Reads `count` 2,352-byte audio sectors from `lba` (little-endian 16-bit stereo). False when the
    // drive reports an error; the buffer then holds nothing usable.
    virtual bool Read(uint32_t lba, unsigned count, uint8_t* out) = 0;
    // The disc's raw CD-Text packs (18 bytes each), or empty.
    virtual std::string CdText() { return {}; }
};

// The optical drives present: "D:" on Windows, "/dev/sr0" on Linux.
std::vector<std::wstring> ListDrives();
std::unique_ptr<Drive> OpenDrive(const std::wstring& name, std::wstring& error);

// The disc's MusicBrainz disc id, and its table of contents as MusicBrainz's toc= parameter.
std::string DiscId(const Toc& toc);
std::string TocParameter(const Toc& toc);

// Names for the disc, as the Rip CD window shows and edits them. `titles` and `artists` have one
// entry per audio track (an empty artist means the album's).
struct Disc {
    std::string source;  // where the names came from: "CD-Text", a release's summary, or empty
    std::string artist, album, date, release, releaseGroup, discId;
    uint32_t number = 0, count = 0;  // disc n of count, 0 = unknown
    std::vector<std::string> titles, artists;
};
// Names from CD-Text packs (block 0, Latin-1 or ASCII). False when the packs name nothing.
bool ParseCdText(std::string_view packs, size_t tracks, Disc& out);
// A MusicBrainz release's medium for this disc as names, padded or cut to `tracks`.
Disc FromRelease(const albumtag::Release& release, size_t tracks);
// Worker thread: asks MusicBrainz about the disc id (falling back to its table of contents), best
// match first. `offline` is set when MusicBrainz couldn't be reached.
std::vector<Disc> LookUp(const Toc& toc, online::Cancel& cancel, bool& offline, const albumtag::Fetch& fetch = nullptr);
// Worker thread: the release's front cover from the Cover Art Archive, or empty.
std::string FetchCover(const std::string& release, online::Cancel& cancel, const albumtag::Fetch& fetch = nullptr);

// Reading: what the two passes found.
struct ReadStats {
    uint32_t reread = 0;  // sectors whose first two reads differed, or failed, and were read again
    uint32_t unsure = 0;  // of those, sectors that never read the same way twice (kept as last read)
};
// One audio track as exact 16-bit stereo, verified as described above. Scan runs the first pass;
// reading then runs the second, re-reading as needed. `progress` gets 0..1 across both passes.
// `offset` is the drive's read offset in samples: positive reads later on the disc. Samples before
// the disc's start or after its audio end read as silence.
std::unique_ptr<PcmReader> OpenTrack(Drive& drive, const Toc& toc, size_t track, int offset, ReadStats& stats,
                                     const std::atomic<bool>& cancel, std::function<void(double)> progress = nullptr);
// Runs the first pass of a reader OpenTrack made; false when cancelled.
bool Scan(PcmReader& reader);

struct Job {
    Toc toc;
    Disc disc;
    std::vector<size_t> tracks;  // indices into toc.tracks, in order
    std::wstring folder;         // files go in folder/album artist/album/
    bool flac = true;            // otherwise .cord
    int offset = 0;
    unsigned threads = 1;        // the .cord encoder's
    std::string cover;           // image bytes to embed, or empty
};
// Where track `i` of the job goes: "<folder>/<album artist>/<album>/<NN> <title>.<ext>".
std::wstring TrackPath(const Job& job, size_t i);

struct TrackDone {
    std::wstring path, error;  // error empty: written and verified
    ReadStats stats;
};
struct Result {
    std::vector<TrackDone> tracks;
    bool cancelled = false;
};
// Worker thread: rips the job's tracks in order, each written and verified before the next starts.
// A track that exists already is skipped, never overwritten. `progress` gets the track's index in
// the job and its fraction done.
Result Rip(Drive& drive, const Job& job, const std::atomic<bool>& cancel,
           const std::function<void(size_t, double)>& progress = nullptr);

// The status line once a rip ends, the same on Windows and Linux.
std::wstring DoneText(const Result& result);

}  // namespace cdrip
