// ID3v2 (MP3) and iTunes ilst (MP4 / M4A) tag writing: frame/atom rebuilding, the v2.3 vs v2.4
// encoding split, and the in-place-vs-rewrite decision. Only tag bytes change; audio is never
// re-encoded, and the result is committed atomically (tagwriter.h) so a failure keeps the original.
#include "tagwriter.h"

#include "library.h"
#include "tags.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string_view>
#include <vector>

namespace {

using sv = std::string_view;

inline uint8_t U8(sv s, size_t i) { return i < s.size() ? (uint8_t)s[i] : 0; }
inline uint32_t BE16(sv s, size_t i) { return (uint32_t)U8(s, i) << 8 | U8(s, i + 1); }
inline uint32_t BE32(sv s, size_t i) { return BE16(s, i) << 16 | BE16(s, i + 2); }
inline uint64_t BE64(sv s, size_t i) { return (uint64_t)BE32(s, i) << 32 | BE32(s, i + 4); }
inline uint32_t Syncsafe(sv s, size_t i) {
    return (U8(s, i) & 0x7F) << 21 | (U8(s, i + 1) & 0x7F) << 14 | (U8(s, i + 2) & 0x7F) << 7 | (U8(s, i + 3) & 0x7F);
}
std::string PutSyncsafe(uint32_t v) {
    return {(char)((v >> 21) & 0x7F), (char)((v >> 14) & 0x7F), (char)((v >> 7) & 0x7F), (char)(v & 0x7F)};
}
std::string PutBE16(uint32_t v) { return {(char)(v >> 8), (char)v}; }
std::string PutBE32(uint32_t v) { return {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v}; }
void SetBE32(std::string& s, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) s[at + i] = (char)(v >> (24 - 8 * i));
}
void SetBE64(std::string& s, size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i) s[at + i] = (char)(v >> (56 - 8 * i));
}

bool IsAscii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return (unsigned char)c < 0x80; });
}

std::string Utf16Le(const std::string& utf8) {
    std::wstring w = FromUtf8(utf8);
    std::string out;
    for (wchar_t c : w) {
        out += (char)(c & 0xFF);
        out += (char)(c >> 8);
    }
    return out;
}

// "3/12" -> 3 and 12 (either may be absent).
void ParsePair(const std::string& text, uint32_t& a, uint32_t& b) {
    a = (uint32_t)atoi(text.c_str());
    size_t slash = text.find('/');
    b = slash == std::string::npos ? 0 : (uint32_t)atoi(text.c_str() + slash + 1);
}

std::string ImageMime(const std::string& img) {
    return img.size() >= 4 && img.compare(0, 4, "\x89PNG") == 0 ? "image/png" : "image/jpeg";
}

// ---------------------------------------------------------------------------
// MP3 / ID3v2

struct Id3Frame {
    std::string id, data;
    uint16_t flags = 0;
};

struct Id3Tag {
    int version = 3;
    size_t size = 0;  // bytes the tag occupies at the start of the file (0 = none)
    std::vector<Id3Frame> frames;
};

bool ParseId3(FileSource& input, const TagEdit& e, Id3Tag& tag, std::wstring& error) {
    std::string head;
    if (!input.Read(0, (size_t)std::min<uint64_t>(10, input.Size()), head)) {
        error = L"Couldn't read the file.";
        return false;
    }
    if (head.size() < 10 || head.compare(0, 3, "ID3") != 0) return true;
    tag.version = U8(head, 3);
    uint8_t flags = U8(head, 5);
    if (tag.version < 3 || tag.version > 4) {
        error = L"This file has an ID3v2." + std::to_wstring(tag.version) + L" tag, which can't be edited yet.";
        return false;
    }
    if (flags & 0x80) {
        error = L"This file's ID3 tag uses unsynchronisation, which can't be edited yet.";
        return false;
    }
    tag.size = 10 + Syncsafe(head, 6) + ((flags & 0x10) ? 10 : 0);
    if (tag.size > input.Size()) {
        error = L"The ID3 tag is larger than the file (damaged tag).";
        return false;
    }
    size_t p = 10, end = 10 + Syncsafe(head, 6);
    if (flags & 0x40) {
        std::string ext;
        if (!input.Read(10, 4, ext)) return false;
        p += tag.version == 4 ? Syncsafe(ext, 0) : 4 + BE32(ext, 0);  // skip (and drop) the extended header
    }
    std::string fh;
    while (p + 10 <= end) {
        if (!input.Read(p, 10, fh) || fh[0] == 0) break;  // stop at padding without reading it
        std::string id = fh.substr(0, 4);
        uint32_t len = tag.version == 4 ? Syncsafe(fh, 4) : BE32(fh, 4);
        uint16_t fl = (uint16_t)BE16(fh, 8);
        if (p + 10 + len > end) break;
        // Skip old cover payloads when replacing/removing the cover.
        if (len && !(e.cover && id == "APIC")) {
            std::string data;
            if (!input.Read(p + 10, len, data)) return false;
            tag.frames.push_back({std::move(id), std::move(data), fl});
        }
        p += 10 + len;
    }
    return true;
}

// Text frame payload in the version's encoding: v2.4 UTF-8, v2.3 Latin-1 or UTF-16 with BOM.
std::string Id3String(int version, const std::string& text, bool terminate, char& enc) {
    if (version == 4) {
        enc = 3;
        return text + (terminate ? std::string(1, '\0') : "");
    }
    if (IsAscii(text)) {
        enc = 0;
        return text + (terminate ? std::string(1, '\0') : "");
    }
    enc = 1;
    return "\xFF\xFE" + Utf16Le(text) + (terminate ? std::string(2, '\0') : "");
}

std::string Id3TextFrame(int version, const std::string& text) {
    char enc;
    std::string body = Id3String(version, text, false, enc);
    return std::string(1, enc) + body;
}

// COMM / USLT: encoding, language, an empty description, then the text. The description uses
// the text's encoding (with its own BOM for UTF-16).
std::string Id3LangFrame(int version, const std::string& text) {
    char enc;
    std::string body = Id3String(version, text, false, enc);
    std::string desc = enc == 1 ? std::string("\xFF\xFE\0\0", 4) : std::string(1, '\0');
    return std::string(1, enc) + "eng" + desc + body;
}

// TXXX: encoding, description, terminator, then the value. ReplayGain descriptions are ASCII,
// so v2.3 can stay Latin-1; v2.4 uses UTF-8.
std::string Id3TxxxFrame(int version, const std::string& desc, const std::string& text) {
    if (version == 4) return std::string(1, '\x03') + desc + std::string(1, '\0') + text;
    if (IsAscii(desc) && IsAscii(text)) return std::string(1, '\0') + desc + std::string(1, '\0') + text;
    return std::string(1, '\x01') + "\xFF\xFE" + Utf16Le(desc) + std::string(2, '\0') + "\xFF\xFE" + Utf16Le(text);
}

// True when a COMM / USLT frame has an empty description (the "plain" comment or lyrics).
bool PlainLangFrame(const Id3Frame& f) {
    if (f.data.size() < 4) return true;
    uint8_t enc = U8(f.data, 0);
    sv rest = sv(f.data).substr(4);
    if (enc == 1 || enc == 2) {
        if (rest.size() >= 2 && ((U8(rest, 0) == 0xFF && U8(rest, 1) == 0xFE) || (U8(rest, 0) == 0xFE && U8(rest, 1) == 0xFF)))
            rest.remove_prefix(2);
        return rest.size() >= 2 && rest[0] == 0 && rest[1] == 0;
    }
    return !rest.empty() && rest[0] == 0;
}

// Digits of a TRCK / TPOS frame as text, whatever its encoding ("3/12").
std::string FrameDigits(const Id3Frame& f) {
    std::string s;
    for (size_t i = 1; i < f.data.size(); ++i) {
        char c = f.data[i];
        if ((c >= '0' && c <= '9') || c == '/') s += c;
    }
    return s;
}

std::string Latin1Field(const std::string& utf8, size_t width) {
    std::wstring w = FromUtf8(utf8);
    std::string out(width, '\0');
    int n = WideCharToMultiByte(28591, 0, w.c_str(), (int)w.size(), nullptr, 0, "?", nullptr);
    std::string latin(n, '\0');
    WideCharToMultiByte(28591, 0, w.c_str(), (int)w.size(), latin.data(), n, "?", nullptr);
    memcpy(out.data(), latin.data(), std::min(latin.size(), width));
    return out;
}

// Rebuilds the ID3v2 tag. When the new frames fit the old tag's space it patches in place (the
// slack becomes padding); otherwise it writes a new file with 2 KiB of growth room. A trailing
// ID3v1 tag is updated in parallel so both formats stay consistent.
bool WriteMp3(const std::wstring& path, const TagEdit& e, std::wstring& error) {
    FileSource input(path);
    if (!input.Valid()) {
        error = L"Couldn't read the file.";
        return false;
    }
    Id3Tag tag;
    if (!ParseId3(input, e, tag, error)) return false;
    int ver = tag.version;
    auto& frames = tag.frames;
    auto find = [&](const char* id) {
        return std::find_if(frames.begin(), frames.end(), [&](const Id3Frame& f) { return f.id == id; });
    };
    auto drop = [&](std::initializer_list<const char*> ids) {
        std::erase_if(frames, [&](const Id3Frame& f) {
            return std::any_of(ids.begin(), ids.end(), [&](const char* id) { return f.id == id; });
        });
    };
    // Taggers also store fields as user text frames (TXXX "BPM", "LABEL", ...), which the reader
    // honours too, so an edited field drops those copies.
    auto dropTxxx = [&](std::initializer_list<const char*> keys) {
        std::erase_if(frames, [&](const Id3Frame& f) {
            if (f.id != "TXXX" || f.data.empty()) return false;
            std::string desc;
            int enc = U8(f.data, 0);
            for (size_t i = 1; i < f.data.size(); ++i) {
                char c = f.data[i];
                if ((enc == 1 || enc == 2) && (i == 1 || i == 2) && ((uint8_t)c == 0xFF || (uint8_t)c == 0xFE)) continue;
                if (c == 0) {
                    if (enc == 1 || enc == 2) {  // UTF-16: one zero byte per ASCII char; two end it
                        if (i + 1 < f.data.size() && f.data[i + 1] == 0) break;
                        continue;
                    }
                    break;
                }
                desc += (char)toupper((unsigned char)c);
            }
            return std::any_of(keys.begin(), keys.end(), [&](const char* k) { return desc == k; });
        });
    };
    if (e.albumArtist) dropTxxx({"ALBUMARTIST", "ALBUM ARTIST"});
    if (e.composer) dropTxxx({"COMPOSER"});
    if (e.comment) dropTxxx({"COMMENT", "DESCRIPTION"});
    if (e.grouping) dropTxxx({"GROUPING", "CONTENTGROUP"});
    if (e.label) dropTxxx({"LABEL", "PUBLISHER", "ORGANIZATION"});
    if (e.lyrics) dropTxxx({"LYRICS", "UNSYNCEDLYRICS"});
    if (e.track || e.trackTotal) dropTxxx({"TRACKTOTAL", "TOTALTRACKS"});
    if (e.disc || e.discTotal) dropTxxx({"DISCNUMBER", "DISCTOTAL", "TOTALDISCS"});
    if (e.bpm) dropTxxx({"BPM"});
    if (e.compilation) dropTxxx({"COMPILATION"});
    auto text = [&](const char* id, const std::optional<std::string>& v) {
        if (!v) return;
        drop({id});
        if (!v->empty()) frames.push_back({id, Id3TextFrame(ver, *v), 0});
    };
    auto lang = [&](const char* id, const std::optional<std::string>& v) {
        if (!v) return;
        std::erase_if(frames, [&](const Id3Frame& f) { return f.id == id && PlainLangFrame(f); });
        if (!v->empty()) frames.push_back({id, Id3LangFrame(ver, *v), 0});
    };
    auto pair = [&](const char* id, const std::optional<uint32_t>& a, const std::optional<uint32_t>& b) {
        if (!a && !b) return;
        uint32_t oldA = 0, oldB = 0;
        if (auto it = find(id); it != frames.end()) ParsePair(FrameDigits(*it), oldA, oldB);
        uint32_t na = a ? *a : oldA, nb = b ? *b : oldB;
        drop({id});
        if (na || nb) frames.push_back({id, Id3TextFrame(ver, std::to_string(na) + (nb ? "/" + std::to_string(nb) : "")), 0});
    };
    auto txxx = [&](const char* desc, const std::optional<std::string>& v) {
        if (!v) return;
        dropTxxx({desc});
        if (!v->empty()) frames.push_back({"TXXX", Id3TxxxFrame(ver, desc, *v), 0});
    };

    text("TIT2", e.title);
    text("TPE1", e.artist);
    text("TALB", e.album);
    text("TPE2", e.albumArtist);
    text("TCON", e.genre);
    if (e.date) {
        drop({"TDRC", "TYER", "TDAT", "TIME", "TRDA"});
        if (!e.date->empty()) {
            if (ver == 4) frames.push_back({"TDRC", Id3TextFrame(ver, *e.date), 0});
            else frames.push_back({"TYER", Id3TextFrame(ver, e.date->substr(0, 4)), 0});
        }
    }
    text("TCOM", e.composer);
    lang("COMM", e.comment);
    if (e.grouping) drop({"GRP1"});
    text("TIT1", e.grouping);
    text("TPUB", e.label);
    lang("USLT", e.lyrics);
    pair("TRCK", e.track, e.trackTotal);
    pair("TPOS", e.disc, e.discTotal);
    if (e.bpm) text("TBPM", *e.bpm ? std::optional<std::string>(std::to_string(*e.bpm)) : std::optional<std::string>(""));
    if (e.compilation) text("TCMP", *e.compilation ? std::optional<std::string>("1") : std::optional<std::string>(""));
    txxx("REPLAYGAIN_TRACK_GAIN", e.rgTrackGain);
    txxx("REPLAYGAIN_TRACK_PEAK", e.rgTrackPeak);
    txxx("REPLAYGAIN_ALBUM_GAIN", e.rgAlbumGain);
    txxx("REPLAYGAIN_ALBUM_PEAK", e.rgAlbumPeak);
    // FMPS names are mixed case by its spec; dropTxxx compares upper case.
    auto fmps = [&](const char* desc, const char* upper, const std::optional<std::string>& v) {
        if (!v) return;
        dropTxxx({upper});
        if (!v->empty()) frames.push_back({"TXXX", Id3TxxxFrame(ver, desc, *v), 0});
    };
    fmps("FMPS_Rating", "FMPS_RATING", e.rating);
    fmps("FMPS_Playcount", "FMPS_PLAYCOUNT", e.playCount);
    // Most MP3 players read ratings from POPM instead: an email, then 1..255. cordinal touches only
    // its own frame and uses the common star steps 1, 64, 128, 196, 255.
    if (e.rating) {
        static const char kEmail[] = "cordinal";
        std::erase_if(frames, [](const Id3Frame& f) { return f.id == "POPM" && f.data.compare(0, sizeof kEmail, kEmail, sizeof kEmail) == 0; });
        const int stars = std::clamp((int)std::lround(atof(e.rating->c_str()) * 5), 0, 5);
        static const uint8_t kSteps[] = {0, 1, 64, 128, 196, 255};
        if (!e.rating->empty() && stars) frames.push_back({"POPM", std::string(kEmail, sizeof kEmail) + (char)kSteps[stars], 0});
    }
    if (e.cover) {
        drop({"APIC"});
        if (!e.cover->empty())
            frames.push_back({"APIC", std::string(1, '\0') + ImageMime(*e.cover) + '\0' + '\x03' + '\0' + *e.cover, 0});
    }

    std::string body;
    for (const auto& f : frames) {
        body += f.id;
        body += ver == 4 ? PutSyncsafe((uint32_t)f.data.size()) : PutBE32((uint32_t)f.data.size());
        body += PutBE16(f.flags);
        body += f.data;
    }
    std::vector<Id3Frame>().swap(frames);
    // Keep the old tag's total size when the frames fit, so a full-file rewrite does not grow the
    // file just because the new tag is shorter.
    size_t total = tag.size && 10 + body.size() <= tag.size ? tag.size : 10 + body.size() + 2048;
    size_t padBytes = total - (10 + body.size());
    std::string header = "ID3" + std::string(1, (char)ver) + std::string(2, '\0') + PutSyncsafe((uint32_t)(total - 10));
    std::string newTag = header + body;
    std::string().swap(body);

    // A trailing ID3v1 tag gets the edited title, artist, album and year too.
    std::string v1;
    uint64_t v1At = input.Size();
    if (input.Size() >= 128 && (e.title || e.artist || e.album || e.date)) {
        if (!input.Read(input.Size() - 128, 128, v1)) {
            error = L"Couldn't read the trailing ID3 tag.";
            return false;
        }
        if (v1.compare(0, 3, "TAG") != 0) v1.clear();
    }
    if (!v1.empty()) {
        v1At -= 128;
        if (v1At < tag.size) {
            error = L"The ID3 tags overlap (damaged file).";
            return false;
        }
        if (e.title) v1.replace(3, 30, Latin1Field(*e.title, 30));
        if (e.artist) v1.replace(33, 30, Latin1Field(*e.artist, 30));
        if (e.album) v1.replace(63, 30, Latin1Field(*e.album, 30));
        if (e.date) v1.replace(93, 4, Latin1Field(e.date->substr(0, 4), 4));
    }
    FileRewrite out(path, input, error);
    return out.Write(newTag) && out.Zeros(padBytes) && out.Copy(tag.size, v1At - tag.size) && out.Write(v1) && out.Commit();
}

// ---------------------------------------------------------------------------
// MP4 / ilst

struct Atom {
    std::string type;
    size_t start = 0, header = 8, size = 0;  // size includes the header
    size_t Body() const { return start + header; }
    size_t End() const { return start + size; }
};

// Atoms in [begin, end) of `s`. False if they don't tile the range exactly.
bool ListAtoms(sv s, size_t begin, size_t end, std::vector<Atom>& out) {
    out.clear();
    for (size_t p = begin; p < end;) {
        if (end - p < 8) return false;
        Atom a;
        a.start = p;
        a.type = std::string(s.substr(p + 4, 4));
        uint64_t size = BE32(s, p);
        if (size == 1) {
            if (end - p < 16) return false;
            size = BE64(s, p + 8);
            a.header = 16;
        } else if (size == 0) {
            size = end - p;
        }
        if (size < a.header || size > end - p) return false;
        a.size = (size_t)size;
        out.push_back(a);
        p += a.size;
    }
    return true;
}

// Enumerate top-level atoms from their headers. mdat/free bodies may be gigabytes long.
bool ListFileAtoms(FileSource& file, std::vector<Atom>& out) {
    for (uint64_t p = 0; p < file.Size();) {
        std::string header;
        if (!file.Read(p, 8, header)) return false;
        Atom a{std::string(header.substr(4, 4)), (size_t)p, 8, 0};
        uint64_t bytes = BE32(header, 0);
        if (bytes == 1) {
            if (!file.Read(p, 16, header)) return false;
            bytes = BE64(header, 8);
            a.header = 16;
        } else if (!bytes) {
            bytes = file.Size() - p;
        }
        if (bytes < a.header || bytes > file.Size() - p) return false;
        a.size = (size_t)bytes;
        out.push_back(a);
        p += bytes;
    }
    return true;
}

std::string MakeAtom(const std::string& type, const std::string& body) { return PutBE32((uint32_t)(8 + body.size())) + type + body; }

std::string DataAtom(uint32_t type, const std::string& payload) {
    return MakeAtom("data", PutBE32(type) + PutBE32(0) + payload);
}

// The payload of an item's data atom, if any.
std::string ItemPayload(sv item) {
    std::vector<Atom> kids;
    if (!ListAtoms(item, 8, item.size(), kids)) return {};
    for (auto& k : kids)
        if (k.type == "data" && k.size >= k.header + 8)
            return std::string(item.substr(k.Body() + 8, k.size - k.header - 8));
    return {};
}

// Name of a freeform ("----") item, uppercased.
std::string FreeformName(sv item) {
    std::vector<Atom> kids;
    if (!ListAtoms(item, 8, item.size(), kids)) return {};
    for (auto& k : kids)
        if (k.type == "name" && k.size >= k.header + 4) {
            std::string n(item.substr(k.Body() + 4, k.size - k.header - 4));
            for (auto& c : n) c = (char)toupper((unsigned char)c);
            return n;
        }
    return {};
}

// Rebuilds the ilst from its existing items (kept verbatim unless an edited field replaces them),
// or builds one from scratch for a file with no metadata. Returns the whole ilst atom.
std::string BuildIlst(sv oldIlst, const TagEdit& e) {
    std::vector<std::string> items;
    std::vector<Atom> kids;
    if (!oldIlst.empty() && ListAtoms(oldIlst, 8, oldIlst.size(), kids))
        for (auto& k : kids) items.emplace_back(oldIlst.substr(k.start, k.size));
    auto typeOf = [](const std::string& item) { return item.substr(4, 4); };
    auto findItem = [&](const std::string& type) {
        return std::find_if(items.begin(), items.end(), [&](const std::string& i) { return typeOf(i) == type; });
    };
    auto drop = [&](const std::string& type) { std::erase_if(items, [&](const std::string& i) { return typeOf(i) == type; }); };
    auto text = [&](const char* type, const std::optional<std::string>& v) {
        if (!v) return;
        drop(type);
        if (!v->empty()) items.push_back(MakeAtom(type, DataAtom(1, *v)));
    };
    auto pair = [&](const char* type, const std::optional<uint32_t>& a, const std::optional<uint32_t>& b, size_t len) {
        if (!a && !b) return;
        uint32_t oldA = 0, oldB = 0;
        if (auto it = findItem(type); it != items.end()) {
            std::string p = ItemPayload(*it);
            oldA = BE16(p, 2);
            oldB = BE16(p, 4);
        }
        uint32_t na = a ? *a : oldA, nb = b ? *b : oldB;
        drop(type);
        if (!na && !nb) return;
        std::string payload = PutBE16(0) + PutBE16(na) + PutBE16(nb);
        payload.resize(len, '\0');
        items.push_back(MakeAtom(type, DataAtom(0, payload)));
    };
    auto freeform = [&](const char* name, const std::optional<std::string>& v) {
        if (!v) return;
        std::string upper = name;
        for (auto& c : upper) c = (char)toupper((unsigned char)c);
        std::erase_if(items, [&](const std::string& i) {
            return typeOf(i) == "----" && FreeformName(i) == upper;
        });
        if (!v->empty())
            items.push_back(MakeAtom("----", MakeAtom("mean", PutBE32(0) + "com.apple.iTunes") +
                                                 MakeAtom("name", PutBE32(0) + name) + DataAtom(1, *v)));
    };
    text("\xA9nam", e.title);
    text("\xA9" "ART", e.artist);
    text("\xA9" "alb", e.album);
    text("aART", e.albumArtist);
    if (e.genre) drop("gnre");
    text("\xA9gen", e.genre);
    text("\xA9" "day", e.date);
    text("\xA9wrt", e.composer);
    text("\xA9" "cmt", e.comment);
    text("\xA9grp", e.grouping);
    text("\xA9lyr", e.lyrics);
    pair("trkn", e.track, e.trackTotal, 8);
    pair("disk", e.disc, e.discTotal, 6);
    if (e.bpm) {
        drop("tmpo");
        if (*e.bpm) items.push_back(MakeAtom("tmpo", DataAtom(21, PutBE16(std::min<uint32_t>(*e.bpm, 65535)))));
    }
    if (e.compilation) {
        drop("cpil");
        if (*e.compilation) items.push_back(MakeAtom("cpil", DataAtom(21, std::string(1, '\x01'))));
    }
    if (e.label) {
        std::erase_if(items, [&](const std::string& i) {
            if (typeOf(i) != "----") return false;
            std::string n = FreeformName(i);
            return n == "LABEL" || n == "PUBLISHER";
        });
        if (!e.label->empty())
            items.push_back(MakeAtom("----", MakeAtom("mean", PutBE32(0) + "com.apple.iTunes") +
                                                 MakeAtom("name", PutBE32(0) + "LABEL") + DataAtom(1, *e.label)));
    }
    freeform("REPLAYGAIN_TRACK_GAIN", e.rgTrackGain);
    freeform("REPLAYGAIN_TRACK_PEAK", e.rgTrackPeak);
    freeform("REPLAYGAIN_ALBUM_GAIN", e.rgAlbumGain);
    freeform("REPLAYGAIN_ALBUM_PEAK", e.rgAlbumPeak);
    freeform("FMPS_Rating", e.rating);
    freeform("FMPS_Playcount", e.playCount);
    if (e.cover) {
        drop("covr");
        if (!e.cover->empty())
            items.push_back(MakeAtom("covr", DataAtom(ImageMime(*e.cover) == "image/png" ? 14 : 13, *e.cover)));
    }
    std::string body;
    for (auto& i : items) body += i;
    return MakeAtom("ilst", body);
}

const std::string kHdlr = MakeAtom("hdlr", std::string(8, '\0') + "mdirappl" + std::string(9, '\0'));

// Rebuilds a container atom with `edit` applied to its children. `full` containers (meta)
// carry 4 bytes of version/flags before their children.
std::string RebuildContainer(sv s, const Atom& a, bool full,
                             const std::function<void(std::vector<std::string>& kids, std::vector<std::string>& types)>& edit) {
    size_t childStart = a.Body() + (full ? 4 : 0);
    std::vector<Atom> kids;
    ListAtoms(s, childStart, a.End(), kids);
    std::vector<std::string> raw, types;
    for (auto& k : kids) {
        raw.emplace_back(s.substr(k.start, k.size));
        types.push_back(k.type);
    }
    edit(raw, types);
    std::string body = full ? std::string(s.substr(a.Body(), 4)) : std::string();
    for (auto& r : raw) body += r;
    return MakeAtom(a.type, body);
}

std::string NewMeta(const TagEdit& e) { return MakeAtom("meta", std::string(4, '\0') + kHdlr + BuildIlst({}, e)); }

// Adds `delta` to every chunk offset (stco / co64) in `moov` that points past `after`.
void ShiftChunkOffsets(std::string& moov, size_t begin, size_t end, int64_t delta, uint64_t after) {
    std::vector<Atom> kids;
    if (!ListAtoms(moov, begin, end, kids)) return;
    for (auto& k : kids) {
        if (k.type == "trak" || k.type == "mdia" || k.type == "minf" || k.type == "stbl") {
            ShiftChunkOffsets(moov, k.Body(), k.End(), delta, after);
        } else if (k.type == "stco" || k.type == "co64") {
            bool wide = k.type == "co64";
            size_t p = k.Body() + 4;  // version/flags
            uint32_t n = BE32(moov, p);
            p += 4;
            for (uint32_t i = 0; i < n && p + (wide ? 8 : 4) <= k.End(); ++i, p += wide ? 8 : 4) {
                uint64_t off = wide ? BE64(moov, p) : BE32(moov, p);
                if (off <= after) continue;
                if (wide) SetBE64(moov, p, off + delta);
                else SetBE32(moov, p, (uint32_t)(off + delta));
            }
        }
    }
}

// Checks that every container this writer will rebuild is internally consistent. Without this,
// a malformed child list would make BuildIlst/RebuildContainer start from an empty or partial set
// and silently drop the tags that are still there.
bool ValidateContainers(sv s, size_t begin, size_t end, int depth) {
    if (depth > 64) return false;
    std::vector<Atom> kids;
    if (!ListAtoms(s, begin, end, kids)) return false;
    for (auto& k : kids) {
        size_t child = k.Body();
        if (k.type == "meta") {
            // The writer rebuilds ISO full-box meta (4 bytes of version/flags). Reject a
            // QuickTime-style meta rather than rebuilding it with the wrong layout.
            if (k.size < k.header + 4 || BE32(s, child) != 0) return false;
            child += 4;
        }
        if (k.type == "udta" || k.type == "meta" || k.type == "ilst" || k.type == "trak" || k.type == "mdia" ||
            k.type == "minf" || k.type == "stbl") {
            if (!ValidateContainers(s, child, k.End(), depth + 1)) return false;
        }
    }
    return true;
}

// Rebuilds the moov atom with the new metadata, creating moov/udta/meta/ilst as needed. Since moov
// can sit before mdat, a size change shifts every stco/co64 chunk offset past it — unless a
// following free/skip atom can absorb it, which leaves the media offsets untouched.
bool WriteMp4(const std::wstring& path, const TagEdit& e, std::wstring& error) {
    FileSource input(path);
    if (!input.Valid()) {
        error = L"Couldn't read the file.";
        return false;
    }
    std::vector<Atom> top;
    if (!ListFileAtoms(input, top)) {
        error = L"The file's MP4 structure is damaged.";
        return false;
    }
    auto moovIt = std::find_if(top.begin(), top.end(), [](const Atom& a) { return a.type == "moov"; });
    if (moovIt == top.end()) {
        error = L"No moov atom: not an MP4 file?";
        return false;
    }
    if (std::any_of(top.begin(), top.end(), [](const Atom& a) { return a.type == "moof"; })) {
        error = L"Fragmented MP4 files can't be edited yet.";
        return false;
    }
    Atom moov = *moovIt;
    if (moov.header != 8) {
        error = L"Very large MP4 headers can't be edited yet.";
        return false;
    }
    std::string metadata;
    if (!input.Read(moov.start, moov.size, metadata)) {
        error = L"Couldn't read the MP4 metadata.";
        return false;
    }
    // moov -> udta -> meta -> ilst, creating whatever is missing.
    if (!ValidateContainers(metadata, moov.header, moov.size, 0)) {
        error = L"The MP4 metadata structure is damaged.";
        return false;
    }
    Atom localMoov = moov;
    localMoov.start = 0;
    std::string newMoov = RebuildContainer(metadata, localMoov, false, [&](auto& kids, auto& types) {
        auto u = std::find(types.begin(), types.end(), "udta");
        if (u == types.end()) {
            kids.push_back(MakeAtom("udta", NewMeta(e)));
            return;
        }
        std::string& udta = kids[u - types.begin()];
        Atom ua{"udta", 0, 8, udta.size()};
        udta = RebuildContainer(udta, ua, false, [&](auto& ukids, auto& utypes) {
            auto m = std::find(utypes.begin(), utypes.end(), "meta");
            if (m == utypes.end()) {
                ukids.push_back(NewMeta(e));
                return;
            }
            std::string& meta = ukids[m - utypes.begin()];
            Atom ma{"meta", 0, 8, meta.size()};
            meta = RebuildContainer(meta, ma, true, [&](auto& mkids, auto& mtypes) {
                if (std::find(mtypes.begin(), mtypes.end(), "hdlr") == mtypes.end()) {
                    mkids.insert(mkids.begin(), kHdlr);
                    mtypes.insert(mtypes.begin(), "hdlr");
                }
                auto i = std::find(mtypes.begin(), mtypes.end(), "ilst");
                if (i == mtypes.end()) mkids.push_back(BuildIlst({}, e));
                else mkids[i - mtypes.begin()] = BuildIlst(mkids[i - mtypes.begin()], e);
            });
        });
    });
    std::string().swap(metadata);

    int64_t delta = (int64_t)newMoov.size() - (int64_t)moov.size;
    size_t tailStart = moov.End();
    bool dataAfter = std::any_of(top.begin(), top.end(), [&](const Atom& a) { return a.type == "mdat" && a.start > moov.start; });
    uint64_t padding = 0;
    if (dataAfter && delta != 0) {
        // Absorb the change in a free atom right after moov when there is one, so no media
        // offsets move; otherwise shift every chunk offset past moov.
        auto next = moovIt + 1;
        if (next != top.end() && (next->type == "free" || next->type == "skip") && next->header == 8 &&
            ((int64_t)next->size - delta >= 8 || (int64_t)next->size == delta)) {
            int64_t left = (int64_t)next->size - delta;
            padding = (uint64_t)left;
            tailStart = next->End();
        } else {
            ShiftChunkOffsets(newMoov, 8, newMoov.size(), delta, moov.start);
        }
    }
    // Padding is emitted in chunks too; a large free atom must not become a RAM allocation.
    FileRewrite out(path, input, error);
    return out.Copy(0, moov.start) && out.Write(newMoov) &&
           (!padding || (out.Write(PutBE32((uint32_t)padding) + "free") && out.Zeros(padding - 8))) &&
           out.Copy(tailStart, input.Size() - tailStart) && out.Commit();
}

}  // namespace

bool WriteTagsMp3(const std::wstring& path, const TagEdit& edit, std::wstring& error) { return WriteMp3(path, edit, error); }
bool WriteTagsMp4(const std::wstring& path, const TagEdit& edit, std::wstring& error) { return WriteMp4(path, edit, error); }
