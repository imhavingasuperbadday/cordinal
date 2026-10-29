// Tag writing for cordinal's own format (cord.h): META's typed fields and PICT pictures. The edit
// replaces the entries of the fields it sets, every other entry and chunk is kept as it was, and
// the file is rebuilt through FileRewrite with the old metadata space reused as padding when the
// new tags fit, so the file keeps its size and a failed write leaves it untouched (tagwriter.h).
#include "tagwriter.h"

#include "cord.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
using namespace cord;

// The edit applied to META's entries: an edited field loses its old entries and gains the new
// value unless that is empty or 0. Other entries, free-form ones included, keep their order;
// the statistics are free-form entries under their Vorbis comment keys, as a conversion stores them.
bool EditEntries(std::string_view old, const TagEdit& e, std::string& out, std::wstring& error) {
    std::string added;
    std::vector<uint8_t> edited;
    auto text = [&](uint8_t id, const std::optional<std::string>& v) {
        if (!v) return;
        edited.push_back(id);
        if (!v->empty()) PutEntry(added, id, {}, *v);
    };
    auto number = [&](uint8_t id, const std::optional<uint32_t>& v) {
        if (!v) return;
        edited.push_back(id);
        if (*v) {
            std::string bytes;
            PutLe(bytes, *v, 4);
            PutEntry(added, id, {}, bytes);
        }
    };
    // ReplayGain arrives as text ("-6.50 dB", "0.988000") and is stored as a float.
    auto gain = [&](uint8_t id, const std::optional<std::string>& v) {
        if (!v) return true;
        edited.push_back(id);
        if (v->empty()) return true;
        char* end = nullptr;
        const float value = strtof(v->c_str(), &end);
        if (end == v->c_str() || !std::isfinite(value)) return false;
        uint32_t u;
        memcpy(&u, &value, 4);
        std::string bytes;
        PutLe(bytes, u, 4);
        PutEntry(added, id, {}, bytes);
        return true;
    };
    text(kTitle, e.title);
    text(kArtist, e.artist);
    text(kAlbum, e.album);
    text(kAlbumArtist, e.albumArtist);
    text(kGenre, e.genre);
    text(kDate, e.date);
    text(kComposer, e.composer);
    text(kComment, e.comment);
    text(kGrouping, e.grouping);
    text(kLabel, e.label);
    text(kLyrics, e.lyrics);
    number(kTrack, e.track);
    number(kTrackTotal, e.trackTotal);
    number(kDisc, e.disc);
    number(kDiscTotal, e.discTotal);
    number(kBpm, e.bpm);
    if (e.compilation) {
        edited.push_back(kCompilation);
        if (*e.compilation) PutEntry(added, kCompilation, {}, std::string(1, '\x01'));
    }
    if (!gain(kTrackGain, e.rgTrackGain) || !gain(kTrackPeak, e.rgTrackPeak) || !gain(kAlbumGain, e.rgAlbumGain) ||
        !gain(kAlbumPeak, e.rgAlbumPeak)) {
        error = L"A ReplayGain value isn't a number.";
        return false;
    }
    std::vector<std::string_view> editedKeys;
    auto freeForm = [&](std::string_view key, const std::optional<std::string>& v) {
        if (!v) return;
        editedKeys.push_back(key);
        if (!v->empty()) PutEntry(added, kFreeForm, key, *v);
    };
    freeForm("FMPS_RATING", e.rating);
    freeForm("FMPS_PLAYCOUNT", e.playCount);
    auto sameKey = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return toupper((unsigned char)x) == y; });
    };
    out.clear();
    size_t at = 0;
    Entry entry;
    while (NextEntry(old, at, entry))
        if (entry.id == kFreeForm ? std::none_of(editedKeys.begin(), editedKeys.end(), [&](std::string_view k) { return sameKey(entry.key, k); })
                                  : std::find(edited.begin(), edited.end(), entry.id) == edited.end())
            PutEntry(out, entry.id, entry.key, entry.value);
    if (at != old.size()) {
        error = L"The file's tags are damaged.";
        return false;
    }
    out += added;
    return true;
}

bool WriteCord(const std::wstring& path, const TagEdit& e, std::wstring& error) {
    FileSource input(path);
    std::string bytes;
    if (!input.Valid() || !input.Read(0, kHeaderBytes, bytes) || bytes.size() < kHeaderBytes ||
        !IsHeader((const uint8_t*)bytes.data())) {
        error = L"Not a .cord file this version of cordinal can edit.";
        return false;
    }
    struct Chunk {
        uint32_t id;
        uint64_t at, bytes;  // where its header starts, and its payload size
    };
    std::vector<Chunk> chunks;
    for (uint64_t at = kHeaderBytes; at < input.Size();) {
        if (input.Size() - at < kChunkHeaderBytes || !input.Read(at, kChunkHeaderBytes, bytes)) {
            error = L"The file is damaged.";
            return false;
        }
        const Chunk chunk{Le32((const uint8_t*)bytes.data()), at, Le64((const uint8_t*)bytes.data() + 4)};
        if (chunk.bytes > input.Size() - at - kChunkHeaderBytes) {
            error = L"The file is damaged.";
            return false;
        }
        chunks.push_back(chunk);
        at += kChunkHeaderBytes + chunk.bytes;
    }
    // Tags live between INFO and INDX; INDX, AUDI and anything after them are copied as they are.
    const auto index = std::find_if(chunks.begin(), chunks.end(), [](const Chunk& c) { return c.id == kIndex; });
    if (chunks.empty() || chunks[0].id != kInfo || index == chunks.end()) {
        error = L"The file is damaged.";
        return false;
    }
    std::string meta;
    const auto oldMeta = std::find_if(chunks.begin() + 1, index, [](const Chunk& c) { return c.id == kMeta; });
    if (oldMeta != index && !input.Read(oldMeta->at + kChunkHeaderBytes, (size_t)oldMeta->bytes, meta)) {
        error = L"Couldn't read the file's tags.";
        return false;
    }
    std::string entries;
    if (!EditEntries(meta, e, entries, error)) return false;
    // The new metadata: META, the chunks kept (pictures unless replaced, and any this version
    // doesn't know), then the new cover.
    std::vector<Chunk> kept;
    for (auto c = chunks.begin() + 1; c != index; ++c)
        if (c->id != kMeta && c->id != kPad && !(c->id == kPicture && e.cover)) kept.push_back(*c);
    const std::string cover = e.cover && !e.cover->empty() ? PictureBytes(*e.cover) : std::string();
    uint64_t size = (entries.empty() ? 0 : kChunkHeaderBytes + entries.size()) + (cover.empty() ? 0 : kChunkHeaderBytes + cover.size());
    for (const Chunk& c : kept) size += kChunkHeaderBytes + c.bytes;
    const uint64_t infoEnd = chunks[0].at + kChunkHeaderBytes + chunks[0].bytes, room = index->at - infoEnd;
    const bool exact = size == room, fits = size + kChunkHeaderBytes <= room;
    const uint64_t pad = fits ? room - size - kChunkHeaderBytes : 4096;
    FileRewrite out(path, input, error);
    if (!out.Copy(0, infoEnd)) return false;
    if (!entries.empty() && (!out.Write(ChunkHeader(kMeta, entries.size())) || !out.Write(entries))) return false;
    for (const Chunk& c : kept)
        if (!out.Copy(c.at, kChunkHeaderBytes + c.bytes)) return false;
    if (!cover.empty() && (!out.Write(ChunkHeader(kPicture, cover.size())) || !out.Write(cover))) return false;
    if (!exact && (!out.Write(ChunkHeader(kPad, pad)) || !out.Zeros(pad))) return false;
    return out.Copy(index->at, input.Size() - index->at) && out.Commit();
}

}  // namespace

bool WriteTagsCord(const std::wstring& path, const TagEdit& edit, std::wstring& error) { return WriteCord(path, edit, error); }
