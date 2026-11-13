// The tag reader's FLAC, cord and Ogg (tags_read.h): FLAC's metadata blocks, cordinal's own INFO, META and
// PICT chunks (cord.h), and the identification and comment packets of Ogg Vorbis, Opus, FLAC and Theora.
#include "tags_read.h"

#include "cord.h"

#include <cmath>
#include <cstring>

namespace tagread {

// --- FLAC ------------------------------------------------------------------------------------------

bool ReadFlac(const File& f, uint64_t pos, TagData& t, ArtSink& art) {
    t.codec = Codec::FLAC;
    for (int guard = 0; guard < 256; ++guard) {
        std::string h = f.Read(pos, 4);
        if (h.size() < 4) break;
        bool last = U8(h, 0) & 0x80;
        int type = U8(h, 0) & 0x7F;
        uint32_t len = BE24(h, 1);
        if (type == 0 || type == 4 || (type == 6 && art.WantsImage())) {
            std::string body = f.Read(pos + 4, len);
            if (type == 0 && body.size() >= 18) {
                t.sampleRate = BE24(body, 10) >> 4;
                t.channels = ((U8(body, 12) >> 1) & 7) + 1;
                uint64_t total = (uint64_t)(U8(body, 13) & 0x0F) << 32 | BE32(body, 14);
                if (t.sampleRate) t.duration = (double)total / t.sampleRate;
                if (body.size() >= 34) t.audioHash = BE64(body, 18);  // STREAMINFO's MD5 starts at byte 18
            } else if (type == 4) {
                ParseVorbisComments(body, t, art);
            } else if (type == 6) {
                sv img;
                int ptype = ParseFlacPicture(body, img);
                if (ptype >= 0) art.Offer(ptype, img);
            }
        }
        pos += 4 + (uint64_t)len;
        if (last) break;
    }
    if (t.duration > 0) t.bitrate = Kbps(f.Size(), t.duration);
    return t.sampleRate != 0;
}

// --- cordinal's own format (cord.h) ----------------------------------------------------------------
// INFO, then the typed and free-form entries of META and the PICT pictures, all before INDX.

static void ParseCordMeta(sv meta, TagData& t, ArtSink& art) {
    size_t at = 0;
    cord::Entry e;
    while (cord::NextEntry(meta, at, e)) {
        const std::string text(e.value);
        const uint32_t number = e.value.size() == 4 ? LE32(e.value, 0) : 0;
        float gain = NAN;
        if (e.value.size() == 4) memcpy(&gain, e.value.data(), 4);
        if (!std::isfinite(gain)) gain = NAN;
        switch (e.id) {
        case cord::kFreeForm: ApplyComment(e.key, e.value, t, art); break;
        case cord::kTitle: AppendValue(t.title, text); break;
        case cord::kArtist: AppendValue(t.artist, text); break;
        case cord::kAlbum: AppendValue(t.album, text); break;
        case cord::kAlbumArtist: AppendValue(t.albumArtist, text); break;
        case cord::kGenre: AppendValue(t.genre, text); break;
        case cord::kDate: SetIfEmpty(t.date, text.substr(0, 10)); break;
        case cord::kComposer: AppendValue(t.composer, text); break;
        case cord::kComment: AppendValue(t.comment, text); break;
        case cord::kGrouping: AppendValue(t.grouping, text); break;
        case cord::kLabel: AppendValue(t.label, text); break;
        case cord::kLyrics: if (art.lyrics) *art.lyrics = text; break;
        case cord::kTrack: t.track = number; break;
        case cord::kTrackTotal: t.trackTotal = number; break;
        case cord::kDisc: t.disc = number; break;
        case cord::kDiscTotal: t.discTotal = number; break;
        case cord::kBpm: t.bpm = number; break;
        case cord::kCompilation: t.compilation = !e.value.empty() && e.value[0]; break;
        case cord::kTrackGain: t.rgTrackGain = gain; break;
        case cord::kTrackPeak: t.rgTrackPeak = gain; break;
        case cord::kAlbumGain: t.rgAlbumGain = gain; break;
        case cord::kAlbumPeak: t.rgAlbumPeak = gain; break;
        default: break;  // a field a later version added
        }
    }
}

bool ReadCord(const File& f, TagData& t, ArtSink& art) {
    t.codec = Codec::Cord;
    bool info = false, meta = false;
    for (uint64_t at = cord::kHeaderBytes; at + cord::kChunkHeaderBytes <= f.Size();) {
        std::string h = f.Read(at, cord::kChunkHeaderBytes);
        const uint32_t id = LE32(h, 0);
        const uint64_t bytes = LE64(h, 4), body = at + cord::kChunkHeaderBytes;
        if (h.size() < cord::kChunkHeaderBytes || bytes > f.Size() - body || id == cord::kIndex) break;
        if (!info) {
            cord::Info i;
            std::string b = f.Read(body, (size_t)std::min<uint64_t>(bytes, 256));
            if (id != cord::kInfo || !cord::ParseInfo((const uint8_t*)b.data(), b.size(), i)) return false;
            info = true;
            t.sampleRate = i.rate;
            t.channels = i.channels;
            t.duration = (double)i.frames / i.rate;
            t.audioHash = BE64(sv((const char*)i.md5.data(), 16), 0);  // as FLAC's: the MD5's first 8 bytes
        } else if (id == cord::kMeta && !meta) {
            meta = true;
            ParseCordMeta(f.Read(body, (size_t)bytes), t, art);
        } else if (id == cord::kPicture && art.WantsImage()) {
            std::string b = f.Read(body, (size_t)bytes);
            uint8_t type = 0;
            sv image;
            if (cord::ParsePicture(b, type, image)) art.Offer(type, image);
        }
        at = body + bytes;
    }
    if (t.duration > 0) t.bitrate = Kbps(f.Size(), t.duration);
    return info;
}

// --- Ogg -------------------------------------------------------------------------------------------

bool ReadOgg(const File& f, TagData& t, ArtSink& art) {
    // Collect the first two packets.
    std::string packets[2];
    int index = 0;
    uint64_t pos = 0;
    // A page's header and its segment table (at most 255 entries) come in one read, its body in a second.
    for (int page = 0; page < 256 && index < 2; ++page) {
        std::string h = f.Read(pos, 27 + 255);
        if (h.size() < 27 || h.compare(0, 4, "OggS") != 0) break;
        int segs = U8(h, 26);
        sv table = Sub(h, 27, (size_t)segs);
        size_t bodyLen = 0;
        for (unsigned char s : table) bodyLen += s;
        std::string body = f.Read(pos + 27 + segs, bodyLen);
        size_t off = 0;
        for (int s = 0; s < segs && index < 2; ++s) {
            uint8_t len = U8(table, (size_t)s);
            packets[index].append(Sub(body, off, len));
            off += len;
            if (len < 255) ++index;
        }
        pos += 27 + (uint64_t)segs + bodyLen;
    }
    sv id = packets[0], comments = packets[1];
    uint32_t granuleRate = 0;
    uint64_t preSkip = 0;
    if (id.substr(0, 7) == "\x01vorbis") {
        t.codec = Codec::Vorbis;
        t.channels = U8(id, 11);
        t.sampleRate = granuleRate = LE32(id, 12);
        if (comments.substr(0, 7) == "\x03vorbis") ParseVorbisComments(comments.substr(7), t, art);
    } else if (id.substr(0, 8) == "OpusHead") {
        t.codec = Codec::Opus;
        t.channels = U8(id, 9);
        preSkip = LE16(id, 10);
        t.sampleRate = LE32(id, 12) ? LE32(id, 12) : 48000;
        granuleRate = 48000;
        if (comments.substr(0, 8) == "OpusTags") ParseVorbisComments(comments.substr(8), t, art);
    } else if (id.substr(0, 5) == "\x7F" "FLAC") {
        t.codec = Codec::FLAC;
        // Packet: 0x7F "FLAC", version (2), header count (2), "fLaC", the STREAMINFO block header
        // (4 bytes at 13), then STREAMINFO itself at 17.
        sv info = Sub(id, 17);
        t.sampleRate = granuleRate = BE24(info, 10) >> 4;
        t.channels = ((U8(info, 12) >> 1) & 7) + 1;
        if (info.size() >= 34) t.audioHash = BE64(info, 18);
        if (!comments.empty()) ParseVorbisComments(Sub(comments, 4), t, art);
    } else if (id.substr(0, 7) == "\x80theora") {
        t.codec = Codec::Theora;
        t.video = true;
    } else {
        return false;
    }
    // Duration from the last page's granule position.
    uint64_t size = f.Size();
    std::string tail = f.Read(size > 65536 ? size - 65536 : 0, 65536);
    size_t last = tail.rfind("OggS");
    if (last != std::string::npos && granuleRate) {
        uint64_t granule = LE64(tail, last + 6);
        if (granule != ~0ull && granule > preSkip) t.duration = (double)(granule - preSkip) / granuleRate;
    }
    if (t.duration > 0) t.bitrate = Kbps(size, t.duration);
    return true;
}

}  // namespace tagread
