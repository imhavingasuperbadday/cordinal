// Vorbis-comment writing for FLAC and Ogg (Vorbis, Opus), plus the FLAC PICTURE block and Ogg
// page repagination it needs. FLAC rewrites its whole metadata section; Ogg keeps page 0 (the
// identification header) untouched and only rebuilds the comment packet's pages, renumbering and
// re-CRCing the audio pages after it. Both commit atomically through FileRewrite (tagwriter.h).
#include "tagwriter.h"

#include "library.h"
#include "tags.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace {

using sv = std::string_view;

inline uint8_t U8(sv s, size_t i) { return i < s.size() ? (uint8_t)s[i] : 0; }
inline uint32_t BE24(sv s, size_t i) { return (uint32_t)U8(s, i) << 16 | (uint32_t)U8(s, i + 1) << 8 | U8(s, i + 2); }
inline uint32_t LE32(sv s, size_t i) {
    return U8(s, i) | (uint32_t)U8(s, i + 1) << 8 | (uint32_t)U8(s, i + 2) << 16 | (uint32_t)U8(s, i + 3) << 24;
}
std::string PutLE32(uint32_t v) { return {(char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24)}; }
std::string PutBE32(uint32_t v) { return {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v}; }
std::string PutBE24(uint32_t v) { return {(char)(v >> 16), (char)(v >> 8), (char)v}; }

std::string Upper(sv s) {
    std::string r(s);
    for (auto& c : r) c = (char)toupper((unsigned char)c);
    return r;
}

std::string Base64(sv data) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = (uint32_t)U8(data, i) << 16 | (uint32_t)U8(data, i + 1) << 8 | U8(data, i + 2);
        out += a[n >> 18 & 63];
        out += a[n >> 12 & 63];
        out += i + 1 < data.size() ? a[n >> 6 & 63] : '=';
        out += i + 2 < data.size() ? a[n & 63] : '=';
    }
    return out;
}

// FLAC PICTURE block body: a front cover with its MIME type (sizes left 0 = unknown).
std::string PictureBlock(const std::string& image) {
    std::string mime = image.size() >= 4 && image.compare(0, 4, "\x89PNG") == 0 ? "image/png" : "image/jpeg";
    return PutBE32(3) + PutBE32((uint32_t)mime.size()) + mime + PutBE32(0) + PutBE32(0) + PutBE32(0) + PutBE32(0) +
           PutBE32(0) + PutBE32((uint32_t)image.size()) + image;
}

// A Vorbis comment list (vendor + KEY=value entries) with the edit applied: entries of edited
// fields are replaced, everything else keeps its order. `inlineCover` puts the picture in a
// METADATA_BLOCK_PICTURE comment (Ogg); FLAC uses PICTURE blocks instead.
bool EditComments(sv old, const TagEdit& e, bool inlineCover, bool opus, std::string& out, size_t& end, std::wstring& error) {
    std::string vendor = "cordinal";
    std::vector<std::string> entries;
    end = 0;
    if (!old.empty()) {
        size_t p = 4 + LE32(old, 0);
        if (p + 4 > old.size()) {
            error = L"The file's comment header is damaged.";
            return false;
        }
        vendor = std::string(old.substr(4, p - 4));
        uint32_t count = LE32(old, p);
        p += 4;
        for (uint32_t i = 0; i < count; ++i) {
            if (p + 4 > old.size() || LE32(old, p) > old.size() - p - 4) {
                error = L"The file's comment header is damaged.";
                return false;
            }
            entries.emplace_back(old.substr(p + 4, LE32(old, p)));
            p += 4 + LE32(old, p);
        }
        end = p;
    }
    auto key = [](const std::string& entry) { return Upper(sv(entry).substr(0, entry.find('='))); };
    auto valueOf = [&](std::initializer_list<const char*> keys) -> std::string {
        for (auto& entry : entries)
            for (const char* k : keys)
                if (key(entry) == k) return entry.substr(entry.find('=') + 1);
        return {};
    };
    auto drop = [&](std::initializer_list<const char*> keys) {
        std::erase_if(entries, [&](const std::string& entry) {
            std::string k = key(entry);
            return std::any_of(keys.begin(), keys.end(), [&](const char* x) { return k == x; });
        });
    };
    auto text = [&](std::initializer_list<const char*> keys, const std::optional<std::string>& v) {
        if (!v) return;
        drop(keys);
        if (!v->empty()) entries.push_back(std::string(*keys.begin()) + "=" + *v);
    };
    // Number + total pairs ("TRACKNUMBER=3/12" or TRACKNUMBER + TRACKTOTAL): written as two fields.
    auto pair = [&](const char* numKey, std::initializer_list<const char*> totalKeys, const std::optional<uint32_t>& a,
                    const std::optional<uint32_t>& b) {
        if (!a && !b) return;
        std::string num = valueOf({numKey}), total = valueOf(totalKeys);
        uint32_t oldA = (uint32_t)atoi(num.c_str()), oldB = (uint32_t)atoi(total.c_str());
        if (size_t slash = num.find('/'); slash != std::string::npos && !oldB) oldB = (uint32_t)atoi(num.c_str() + slash + 1);
        uint32_t na = a ? *a : oldA, nb = b ? *b : oldB;
        drop({numKey});
        drop(totalKeys);
        if (na) entries.push_back(std::string(numKey) + "=" + std::to_string(na));
        if (nb) entries.push_back(std::string(*totalKeys.begin()) + "=" + std::to_string(nb));
    };
    text({"TITLE"}, e.title);
    text({"ARTIST"}, e.artist);
    text({"ALBUM"}, e.album);
    text({"ALBUMARTIST", "ALBUM ARTIST"}, e.albumArtist);
    text({"GENRE"}, e.genre);
    text({"DATE", "YEAR"}, e.date);
    text({"COMPOSER"}, e.composer);
    text({"COMMENT", "DESCRIPTION"}, e.comment);
    text({"GROUPING", "CONTENTGROUP"}, e.grouping);
    text({"LABEL", "PUBLISHER", "ORGANIZATION"}, e.label);
    text({"LYRICS", "UNSYNCEDLYRICS"}, e.lyrics);
    text({"REPLAYGAIN_TRACK_GAIN"}, e.rgTrackGain);
    text({"REPLAYGAIN_TRACK_PEAK"}, e.rgTrackPeak);
    text({"REPLAYGAIN_ALBUM_GAIN"}, e.rgAlbumGain);
    text({"REPLAYGAIN_ALBUM_PEAK"}, e.rgAlbumPeak);
    text({"FMPS_RATING"}, e.rating);
    text({"FMPS_PLAYCOUNT"}, e.playCount);
    if (opus) {
        text({"R128_TRACK_GAIN"}, e.r128TrackGain);
        text({"R128_ALBUM_GAIN"}, e.r128AlbumGain);
    }
    pair("TRACKNUMBER", {"TRACKTOTAL", "TOTALTRACKS"}, e.track, e.trackTotal);
    pair("DISCNUMBER", {"DISCTOTAL", "TOTALDISCS"}, e.disc, e.discTotal);
    if (e.bpm) text({"BPM"}, std::optional<std::string>(*e.bpm ? std::to_string(*e.bpm) : ""));
    if (e.compilation) text({"COMPILATION"}, std::optional<std::string>(*e.compilation ? "1" : ""));
    if (inlineCover && e.cover) {
        drop({"METADATA_BLOCK_PICTURE", "COVERART", "COVERARTMIME"});
        if (!e.cover->empty()) entries.push_back("METADATA_BLOCK_PICTURE=" + Base64(PictureBlock(*e.cover)));
    }
    out = PutLE32((uint32_t)vendor.size()) + vendor + PutLE32((uint32_t)entries.size());
    for (auto& entry : entries) out += PutLE32((uint32_t)entry.size()) + entry;
    return true;
}

// ---------------------------------------------------------------------------
// FLAC

// FLAC metadata is a chain of blocks that must start with STREAMINFO. The comment block is
// rebuilt, replaced pictures are dropped, the other blocks are kept as they were, and any leftover
// space is reused as padding (or 4 KiB added) so the rewrite stays close to the original size.
bool WriteFlac(const std::wstring& path, const TagEdit& e, std::wstring& error) {
    FileSource input(path);
    std::string file;
    if (!input.Read(0, 8, file)) {
        error = L"Couldn't read the file.";
        return false;
    }
    if (file.size() < 8 || file.compare(0, 4, "fLaC") != 0) {
        error = L"Not a FLAC file (an ID3 tag in front of FLAC isn't supported).";
        return false;
    }
    struct Block {
        uint8_t type;
        std::string data;
    };
    std::vector<Block> blocks;
    size_t p = 4;
    for (bool last = false; !last;) {
        if (!input.Read(p, 4, file)) {
            error = L"The FLAC metadata is damaged.";
            return false;
        }
        last = U8(file, 0) & 0x80;
        uint8_t type = U8(file, 0) & 0x7F;
        uint32_t len = BE24(file, 1);
        if (len > input.Size() - p - 4) {
            error = L"The FLAC metadata is damaged.";
            return false;
        }
        std::string data;
        // Padding and pictures being replaced need no body allocation.
        if (type != 1 && !(type == 6 && e.cover) && !input.Read(p + 4, len, data)) {
            error = L"Couldn't read the FLAC metadata.";
            return false;
        }
        blocks.push_back({type, std::move(data)});
        p += 4 + len;
    }
    size_t metaEnd = p;
    if (blocks.empty() || blocks[0].type != 0) {
        error = L"The FLAC file doesn't start with STREAMINFO.";
        return false;
    }
    // Rebuild: STREAMINFO, the edited comments, other blocks as they were, pictures.
    auto vc = std::find_if(blocks.begin(), blocks.end(), [](const Block& b) { return b.type == 4; });
    std::string comments;
    size_t used;
    if (!EditComments(vc != blocks.end() ? sv(vc->data) : sv(), e, false, false, comments, used, error)) return false;
    std::vector<Block> out;
    out.push_back(std::move(blocks[0]));
    out.push_back({4, std::move(comments)});
    for (size_t i = 1; i < blocks.size(); ++i) {
        uint8_t t = blocks[i].type;
        if (t == 4 || t == 1 || (t == 6 && e.cover)) continue;  // comments (rebuilt), padding, replaced pictures
        out.push_back(std::move(blocks[i]));
    }
    if (e.cover && !e.cover->empty()) out.push_back({6, PictureBlock(*e.cover)});
    size_t size = 0;
    for (auto& b : out) {
        if (b.data.size() >= (1u << 24)) {
            error = L"A FLAC metadata block can't exceed 16 MB (the cover is too big).";
            return false;
        }
        size += 4 + b.data.size();
    }
    // Keep existing space when it fits; otherwise add 4 KiB of padding. Commit a complete
    // streamed file so even a failed metadata write cannot leave the original half edited.
    size_t room = metaEnd - 4;
    bool exact = size == room, fits = size + 4 <= room;
    size_t pad = fits ? room - size - 4 : 4096;  // the padding block's size (none when exact)
    FileRewrite rewrite(path, input, error);
    if (!rewrite.Write("fLaC")) return false;
    for (size_t i = 0; i < out.size(); ++i) {
        bool last = i + 1 == out.size() && exact;
        std::string header(1, (char)(out[i].type | (last ? 0x80 : 0)));
        if (!rewrite.Write(header + PutBE24((uint32_t)out[i].data.size())) || !rewrite.Write(out[i].data)) return false;
    }
    if (!exact) {
        // Several old padding blocks can add up to more than the 24-bit block-size limit.
        while (pad >= (1u << 24)) {
            size_t chunk = std::min<size_t>(pad - 4, (1u << 24) - 1);
            if (!rewrite.Write(std::string(1, '\x01') + PutBE24((uint32_t)chunk)) || !rewrite.Zeros(chunk)) return false;
            pad -= chunk + 4;
        }
        if (!rewrite.Write(std::string(1, '\x81') + PutBE24((uint32_t)pad)) || !rewrite.Zeros(pad)) return false;
    }
    return rewrite.Copy(metaEnd, input.Size() - metaEnd) && rewrite.Commit();
}

// ---------------------------------------------------------------------------
// Ogg

// Ogg's CRC-32 variant (poly 0x04C11DB7, no input/output reflection, init 0). Every page stores
// one over its entire body, header and all, so any page whose bytes change must be re-CRCed.
uint32_t OggCrc(sv data) {
    static uint32_t table[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; ++k) r = r & 0x80000000u ? (r << 1) ^ 0x04C11DB7u : r << 1;
            table[i] = r;
        }
        return true;
    }();
    (void)init;
    uint32_t crc = 0;
    for (unsigned char c : data) crc = (crc << 8) ^ table[((crc >> 24) ^ c) & 0xFF];
    return crc;
}

struct OggPage {
    size_t start = 0, size = 0;  // the whole page in the file
    uint8_t flags = 0;
    uint32_t serial = 0, seq = 0;
    std::vector<uint8_t> lacing;
    size_t dataStart = 0;
};

bool ReadPage(sv f, size_t p, OggPage& pg) {
    if (p + 27 > f.size() || f.substr(p, 4) != "OggS" || U8(f, p + 4) != 0) return false;
    pg.start = p;
    pg.flags = U8(f, p + 5);
    pg.serial = LE32(f, p + 14);
    pg.seq = LE32(f, p + 18);
    uint8_t n = U8(f, p + 26);
    if (p + 27 + n > f.size()) return false;
    pg.lacing.assign(f.begin() + p + 27, f.begin() + p + 27 + n);
    pg.dataStart = p + 27 + n;
    size_t body = 0;
    for (uint8_t l : pg.lacing) body += l;
    if (pg.dataStart + body > f.size()) return false;
    pg.size = 27 + n + body;
    return true;
}

// One Ogg page is at most 65,307 bytes. Retain only this page while rewriting audio.
bool ReadFilePage(FileSource& file, uint64_t offset, std::string& bytes, OggPage& pg) {
    if (!file.Read(offset, 27, bytes) || bytes.substr(0, 4) != "OggS" || U8(bytes, 4) != 0) return false;
    size_t n = U8(bytes, 26);
    if (!file.Read(offset, 27 + n, bytes)) return false;
    size_t total = 27 + n;
    for (size_t i = 27; i < 27 + n; ++i) total += U8(bytes, i);
    return file.Read(offset, total, bytes) && ReadPage(bytes, 0, pg);
}

// Pages for whole packets, starting at sequence `seq`. Header pages carry granule position 0.
std::string Paginate(const std::vector<std::string>& packets, uint32_t serial, uint32_t& seq) {
    std::string out;
    std::vector<uint8_t> lacing;
    std::string body;
    bool continued = false;  // the page being built starts in the middle of a packet
    bool ended = false;      // a packet ends on the page being built
    auto flush = [&] {
        std::string page = "OggS";
        page += '\0';
        page += (char)(continued ? 1 : 0);
        uint64_t granule = ended ? 0 : ~0ull;  // -1: no packet finishes on this page
        for (int i = 0; i < 8; ++i) page += (char)(granule >> (8 * i));
        page += PutLE32(serial) + PutLE32(seq++) + PutLE32(0);
        page += (char)lacing.size();
        page.append(lacing.begin(), lacing.end());
        page += body;
        uint32_t crc = OggCrc(page);
        page.replace(22, 4, PutLE32(crc));
        out += page;
        lacing.clear();
        body.clear();
        ended = false;
    };
    for (const auto& packet : packets) {
        size_t left = packet.size(), at = 0;
        for (;;) {
            if (lacing.size() == 255) {  // page full: the next one continues this packet if it's unfinished
                flush();
                continued = at > 0;
            }
            uint8_t l = (uint8_t)std::min<size_t>(left, 255);
            lacing.push_back(l);
            body.append(packet, at, l);
            at += l;
            left -= l;
            if (l < 255) {  // a lacing value below 255 ends the packet (0 after an exact multiple)
                ended = true;
                break;
            }
        }
    }
    if (!lacing.empty()) flush();
    return out;
}

bool WriteOgg(const std::wstring& path, const TagEdit& e, std::wstring& error) {
    FileSource input(path);
    std::string file;
    if (!input.Valid()) {
        error = L"Couldn't read the file.";
        return false;
    }
    // Collect the header packets: Vorbis has three (identification, comments, setup), Opus two.
    std::vector<std::string> packets;
    std::string partial;
    OggPage pg;
    size_t p = 0, headers = 0, firstAudio = 0;
    uint32_t serial = 0;
    int pages = 0;
    std::string firstPage;
    uint32_t firstSeq = 0;
    while (ReadFilePage(input, p, file, pg)) {
        if (pages == 0) firstPage = file, firstSeq = pg.seq;
        if (pages == 0) serial = pg.serial;
        else if (pg.serial != serial) {
            error = L"Ogg files with several streams can't be edited yet.";
            return false;
        }
        size_t d = pg.dataStart;
        for (uint8_t l : pg.lacing) {
            partial.append(file, d, l);
            d += l;
            if (l < 255) packets.push_back(std::move(partial)), partial.clear();
        }
        ++pages;
        p += pg.size;
        if (packets.size() == 1 && headers == 0) {
            if (packets[0].compare(0, 7, "\x01vorbis") == 0) headers = 3;
            else if (packets[0].compare(0, 8, "OpusHead") == 0) headers = 2;
            else {
                error = L"Only Ogg Vorbis and Opus files can be edited.";
                return false;
            }
            if (pages != 1 || pg.lacing.empty()) {
                error = L"The Ogg identification header isn't alone on its page.";
                return false;
            }
        }
        if (headers && packets.size() >= headers) {
            if (packets.size() > headers || !partial.empty()) {
                error = L"The Ogg headers share a page with audio (non-standard file).";
                return false;
            }
            firstAudio = p;
            break;
        }
    }
    if (!headers || packets.size() < headers) {
        error = L"The Ogg headers are damaged.";
        return false;
    }
    // Rebuild the comment packet.
    bool vorbis = headers == 3;
    std::string& tags = packets[1];
    size_t magic = vorbis ? 7 : 8;
    if (tags.compare(0, magic, vorbis ? "\x03vorbis" : "OpusTags") != 0) {
        error = L"The Ogg comment header is missing.";
        return false;
    }
    std::string comments;
    size_t used;
    if (!EditComments(sv(tags).substr(magic), e, true, !vorbis, comments, used, error)) return false;
    // Vorbis ends the packet with a framing bit; Opus may carry extra data after the list (kept).
    std::string rest = vorbis ? std::string(1, '\x01') : tags.substr(magic + used);
    tags = tags.substr(0, magic) + comments + rest;

    // Page 0 (identification) stays; the other headers are repaginated; audio pages follow with
    // their sequence numbers shifted and CRCs recomputed.
    uint32_t seq = firstSeq + 1;
    FileRewrite out(path, input, error);
    if (!out.Write(firstPage) || !out.Write(Paginate({packets.begin() + 1, packets.end()}, serial, seq))) return false;
    for (size_t q = firstAudio; q < input.Size();) {
        if (!ReadFilePage(input, q, file, pg)) {
            // Existing trailing junk is preserved verbatim as a streamed source range.
            return out.Copy(q, input.Size() - q) && out.Commit();
        }
        if (pg.serial != serial) {
            error = L"Chained Ogg files (several streams one after another) can't be edited yet.";
            return false;
        }
        std::string& page = file;
        page.replace(18, 4, PutLE32(seq++));
        page.replace(22, 4, PutLE32(0));
        page.replace(22, 4, PutLE32(OggCrc(page)));
        if (!out.Write(page)) return false;
        q += pg.size;
    }
    return out.Commit();
}

}  // namespace

bool WriteTagsFlac(const std::wstring& path, const TagEdit& edit, std::wstring& error) { return WriteFlac(path, edit, error); }
bool WriteTagsOgg(const std::wstring& path, const TagEdit& edit, std::wstring& error) { return WriteOgg(path, edit, error); }
