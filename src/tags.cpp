// Native metadata reader (the implementation behind tags.h). Sniffs the container from its magic
// bytes and parses tags and technical info with bounded random-access reads rather than loading
// whole files, so multi-GB media stays cheap. Output text is always UTF-8. This file dispatches and
// holds the public calls; each family of containers has its own file (tags_read.h lists them).
#include "tags_read.h"

#include "cord.h"
#include "cue.h"
#include "game_music.h"
#include "helpers.h"
#include "library.h"
#include "native_audio.h"
#include "sort.h"

#include <cmath>

using namespace tagread;

namespace {

// Dispatches on the file's first bytes. Anything unrecognised falls through to the MPEG parser,
// which scans for a frame sync, so a bare MP3 with no tag still reads.
bool ReadAny(const std::wstring& path, TagData& t, ArtSink& art) {
    File f(path);
    if (!f.Ok()) return false;
    // Sixteen bytes holds every signature below, ASF's GUID the longest. Twelve used to be read, so
    // every file that reached the ASF test (every MP3 does) paid a second read to see it.
    std::string magic = f.Read(0, 16);
    // Empty or truncated files can also turn up in a saved library. There must be at least
    // four bytes before the MP4 probe takes a substring at offset four.
    if (magic.size() < 4) return false;
    sv m = magic;
    bool ok = false;
    if (m.substr(0, 4) == "fLaC") ok = ReadFlac(f, 4, t, art);
    else if (m.size() >= cord::kHeaderBytes && cord::IsHeader((const uint8_t*)m.data())) ok = ReadCord(f, t, art);
    else if (m.substr(0, 4) == "OggS") ok = ReadOgg(f, t, art);
    else if (m.substr(4, 4) == "ftyp" || m.substr(4, 4) == "moov" || m.substr(4, 4) == "mdat" || m.substr(4, 4) == "wide" || m.substr(4, 4) == "free")
        ok = ReadMp4(f, t, art);
    else if (m.substr(0, 4) == "RIFF" || m.substr(0, 4) == "RF64" || m.substr(0, 4) == "BW64") ok = ReadRiff(f, t, art);
    else if (m.substr(0, 4) == "FORM" && (Sub(m, 8, 4) == "8SVX" || Sub(m, 8, 4) == "16SV")) ok = ReadSvxTags(path, t);
    else if (m.substr(0, 4) == "FORM") ok = ReadAiff(f, t, art);
    else if (m.substr(0, 4) == ".snd" || m.substr(0, 4) == "dns.") ok = ReadAu(f, t);
    else if (m.substr(0, 4) == "caff") ok = ReadCaf(f, t);
    else if (m.substr(0, 4) == "Crea" && f.Read(0, 20) == std::string("Creative Voice File\x1A", 20)) ok = ReadVocTags(path, t);
    else if (m.substr(0, 4) == "DSD ") ok = ReadDsf(f, t, art);
    else if (m.substr(0, 4) == "FRM8") ok = ReadDff(f, t, art);
    else if (IsAsf(m)) ok = ReadAsf(f, t, art);
    else if (m.substr(0, 4) == "\x1A\x45\xDF\xA3") ok = ReadMatroska(f, t);
    else if (m.size() >= 10 && m.substr(0, 3) == "ID3" &&
             f.Read(10 + Syncsafe(m, 6) + ((U8(m, 5) & 0x10) ? 10 : 0), 4) == "fLaC") {
        // FLAC with a (non-standard) leading ID3 tag
        uint64_t skip = 10 + Syncsafe(m, 6) + ((U8(m, 5) & 0x10) ? 10 : 0);
        ParseId3v2File(f, 0, t, art);
        ok = ReadFlac(f, skip + 4, t, art);
    } else if (m.substr(0, 4) == "MPCK" || (m.substr(0, 3) == "MP+" && (U8(m, 3) & 15) == 7)) {
        ok = ReadMusepack(f, t, art);
    } else ok = ReadMpeg(f, t, art);
    return ok;
}

// A CUE track: the file's technical info, with the sheet's song tags on top. ReplayGain of the
// whole file doesn't apply to one song, so only its album gain is kept.
bool ReadSubtrackTags(const std::wstring& path, TagData& t) {
    CueSubtrack sub;
    if (!LookupSubtrack(path, sub)) return false;
    ArtSink none;
    TagData file;
    bool ok = ReadAny(sub.audioPath, file, none);
    const CueSheet& sheet = *sub.sheet;
    const CueTrack& track = sub.Track();
    t = file;
    t.audioHash = 0;  // the MD5 is of the whole file
    t.rgTrackGain = t.rgTrackPeak = NAN;
    if (!track.title.empty()) t.title = track.title;
    else t.title = track.number < 10 ? "Track 0" + std::to_string(track.number) : "Track " + std::to_string(track.number);
    if (!track.performer.empty()) t.artist = track.performer;
    else if (!sheet.performer.empty()) t.artist = sheet.performer;
    if (!sheet.performer.empty()) t.albumArtist = sheet.performer;
    if (!sheet.title.empty()) t.album = sheet.title;
    if (!sheet.date.empty()) t.date = sheet.date;
    if (!sheet.genre.empty()) t.genre = sheet.genre;
    if (!sheet.comment.empty()) t.comment = sheet.comment;
    if (!track.songwriter.empty()) t.composer = track.songwriter;
    t.track = (uint32_t)track.number;
    t.trackTotal = (uint32_t)sheet.TrackCount();
    if (sheet.discNumber > 0) t.disc = (uint32_t)sheet.discNumber;
    if (sheet.totalDiscs > 0) t.discTotal = (uint32_t)sheet.totalDiscs;
    // The last track runs to the end of the file.
    t.duration = track.end > 0 ? track.end - track.start : std::max(0.0, file.duration - track.start);
    return ok;
}

}  // namespace

const wchar_t* CodecName(Codec c) {
    static const wchar_t* const names[] = {
        L"", L"MP3", L"MP2", L"FLAC", L"AAC", L"ALAC", L"PCM", L"WMA", L"Vorbis", L"Opus", L"AC3",
        L"H.264", L"HEVC", L"VP8", L"VP9", L"AV1", L"MPEG-4", L"MPEG-2", L"WMV", L"Theora", L"Video",
        L"SPC", L"NSF", L"GBS", L"VGM", L"Cord", L"DSD", L"SID", L"Module", L"MIDI", L"Musepack",
        L"ADX", L"G.722", L"GSM", L"DFPWM", L"G.726",
    };
    size_t i = (size_t)c;
    return i < std::size(names) ? names[i] : L"";
}

bool ReadTags(const std::wstring& path, TagData& out) {
    out = TagData{};
    if (IsGameMusicFile(path)) {  // "file.nsf|3" or a plain file: the emulator reads the song's tags
        std::wstring file;
        int subsong = 1;
        SplitGameMusicPath(path, file, subsong);
        return ReadGameMusicTags(file, subsong, out);
    }
    const std::wstring ext = ExtOf(path);
    if (ext == L"mod" || ext == L"s3m" || ext == L"xm" || ext == L"it") return ReadModuleTags(path, out);
    // Raw codec streams and ADX carry no tags: their decoder gives the length.
    if (ext == L"adx" || ext == L"g722" || ext == L"gsm" || ext == L"dfpwm") {
        std::wstring error;
        auto d = IsSubtrack(path) ? nullptr : OpenDecoder(path, 48000, error);
        if (!d) return false;
        out.codec = ext == L"adx" ? Codec::ADX : ext == L"g722" ? Codec::G722 : ext == L"gsm" ? Codec::GSM : Codec::DFPWM;
        out.duration = d->Duration();
        out.sampleRate = d->Rate();
        File f(path);
        out.channels = ext == L"adx" ? U8(f.Read(7, 1), 0) : 1;  // the ADX header's channel count
        out.bitrate = Kbps(f.Size(), std::max(out.duration, 0.001));
        return out.duration > 0;
    }
    if (ext == L"mid" || ext == L"midi" || ext == L"kar" || ext == L"rmi") return ReadMidiTags(path, out);
    if (HelperForExtension(ext)) return ReadHelperTags(path, out);  // a file type a helper program plays
    ArtSink none;
    bool ok = IsSubtrack(path) ? ReadSubtrackTags(path, out) : ReadAny(path, out, none);
    if (out.title.empty() && out.artist.empty()) out.title.clear();
    return ok;
}

bool ReadEmbeddedArt(const std::wstring& path, std::string& image) {
    TagData t;
    ArtSink art;
    art.image = &image;
    image.clear();
    ReadAny(AudioFileOf(path), t, art);  // a CUE track's cover is its file's
    return !image.empty();
}

bool ReadLyrics(const std::wstring& path, std::string& lyrics) {
    TagData t;
    ArtSink art;
    art.lyrics = &lyrics;
    lyrics.clear();
    ReadAny(AudioFileOf(path), t, art);
    return !lyrics.empty();
}

bool ReadChapters(const std::wstring& path, std::vector<Chapter>& chapters) {
    chapters.clear();
    TagData t;
    ArtSink art;
    std::map<int, Chapter> found;
    art.chapters = &found;
    ReadAny(AudioFileOf(path), t, art);
    for (auto& [n, c] : found)
        if (c.start >= 0) chapters.push_back(std::move(c));
    SortObjects(chapters.begin(), chapters.end(), [](const Chapter& a, const Chapter& b) { return a.start < b.start; });
    return !chapters.empty();
}

std::wstring ChapterTextAt(const std::vector<Chapter>& chapters, double position) {
    if (chapters.empty()) return {};
    size_t index = 0;
    while (index + 1 < chapters.size() && chapters[index + 1].start <= position) ++index;
    std::wstring title = FromUtf8(chapters[index].title);
    return L"Chapter " + std::to_wstring(index + 1) + L"/" + std::to_wstring(chapters.size()) +
           (title.empty() ? L"" : L": " + title);
}

double ChapterStartAfter(const std::vector<Chapter>& chapters, double position) {
    for (const Chapter& chapter : chapters)
        if (chapter.start > position + 0.25) return chapter.start;
    return 0;
}

bool ReadLoopPoints(const std::wstring& path, int64_t& loopStart, int64_t& loopEnd) {
    File f(path);
    if (!f.Ok()) return false;
    std::string magic = f.Read(0, 4);
    LoopTags tags;
    ArtSink art;
    art.loops = &tags;
    TagData t;
    if (magic == "fLaC") ReadFlac(f, 4, t, art);
    else if (magic == "cord") ReadCord(f, t, art);
    else if (magic == "OggS") ReadOgg(f, t, art);
    else if (magic == "RIFF" || magic == "RF64" || magic == "BW64") ReadRiff(f, t, art);
    else return false;
    int64_t start = 0, end = 0;
    if (tags.wav) {
        start = tags.wavStart;
        end = tags.wavEnd;
    } else {
        // Decimal digits only: a negative or garbled value is no loop point at all.
        auto parse = [](const std::string& text, int64_t& value) {
            std::string s = Trim(text);
            if (s.empty() || s.size() > 18) return false;
            int64_t v = 0;
            for (char c : s) {
                if (c < '0' || c > '9') return false;
                v = v * 10 + (c - '0');
            }
            value = v;
            return true;
        };
        int64_t length = 0;
        if (!parse(tags.start, start)) return false;
        if (!parse(tags.end, end)) {
            if (!parse(tags.length, length)) return false;
            end = start + length;
        }
    }
    if (start < 0 || end <= start) return false;
    loopStart = start;
    loopEnd = end;
    return true;
}
