// Rewriting a fragmented MP4 into one standard fast-start M4A (mp4_remux.h): the
// container audio_mp4.cpp's parser read is rebuilt around the same compressed frames,
// byte for byte, with a full stts/stsc/stsz/stco sample table and the source's own
// sample description, timescales and edit list. The rewrite never re-encodes and never
// reinterprets; it only re-states what the parser measured, and it reads the result back
// before handing it over, so a failed rewrite leaves nothing behind.
#include "mp4_remux.h"

#include "audio_file.h"     // File, U16/U32
#include "audio_packets.h"  // PacketFile

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nativeaudio {
namespace {

void Put16(std::string& out, uint16_t v) {
    out.push_back(char(v >> 8));
    out.push_back(char(v));
}
void Put32(std::string& out, uint32_t v) {
    out.push_back(char(v >> 24));
    out.push_back(char(v >> 16));
    out.push_back(char(v >> 8));
    out.push_back(char(v));
}
void Put64(std::string& out, uint64_t v) {
    Put32(out, uint32_t(v >> 32));
    Put32(out, uint32_t(v));
}

std::string Box(const char* type, const std::string& body) {
    std::string out;
    Put32(out, uint32_t(8 + body.size()));
    out.append(type, 4);
    out += body;
    return out;
}

// A full box: version and flags above the payload.
std::string Full(const char* type, unsigned version, uint32_t flags, const std::string& body) {
    std::string head;
    Put32(head, (uint32_t(version) << 24) | (flags & 0xFFFFFF));
    return Box(type, head + body);
}

std::string Matrix() {
    std::string m;
    Put32(m, 0x00010000); Put32(m, 0); Put32(m, 0);
    Put32(m, 0); Put32(m, 0x00010000); Put32(m, 0);
    Put32(m, 0); Put32(m, 0); Put32(m, 0x40000000);
    return m;
}

// Media ticks to movie ticks. `up` rounds up so a length computed for a track that never
// stated one can never clip the last sample it covers.
bool ScaleUp(uint64_t ticks, uint64_t from, uint64_t to, bool up, uint64_t& out) {
    if (!from || !to || ticks > UINT64_MAX / to) return false;
    const uint64_t scaled = ticks * to;
    uint64_t whole = scaled / from;
    if (up ? scaled % from != 0 : scaled % from >= (from + 1) / 2) ++whole;
    out = whole;
    return true;
}

bool WriteAll(HANDLE h, const void* data, size_t bytes) {
    for (size_t at = 0; at < bytes;) {
        DWORD chunk = (DWORD)std::min<size_t>(bytes - at, 1u << 24), wrote = 0;
        if (!WriteFile(h, (const char*)data + at, chunk, &wrote, nullptr) || wrote != chunk) return false;
        at += chunk;
    }
    return true;
}
bool WriteAll(HANDLE h, const std::string& bytes) { return WriteAll(h, bytes.data(), bytes.size()); }

// Chunks of this many samples keep the offset table small while staying a layout every
// demuxer already knows, rather than one giant chunk few writers ever produce.
constexpr size_t kPerChunk = 1024;

// Sample-table boxes that do not depend on where the data lands; the chunk-offset table,
// which does, is written with the rest of the movie.
struct SampleTables {
    std::string stsd, stts, stsc, stsz;
    size_t chunks = 0;
};

SampleTables BuildTables(const Mp4Fragments& frag) {
    SampleTables t;
    // stsd: the one sample description, copied from the source as it is.
    std::string stsdBody;
    Put32(stsdBody, 1);  // one entry
    stsdBody.append(reinterpret_cast<const char*>(frag.sampleDesc.data()), frag.sampleDesc.size());
    t.stsd = Full("stsd", 0, 0, stsdBody);

    // stts: run-length coded durations; a constant-clock track collapses to one entry.
    std::vector<std::pair<uint32_t, uint32_t>> durations;  // count, delta
    for (const auto& run : frag.runs) {
        if (!durations.empty() && durations.back().second == run.frames) ++durations.back().first;
        else durations.push_back({1, run.frames});
    }
    std::string sttsBody;
    Put32(sttsBody, uint32_t(durations.size()));
    for (const auto& [count, delta] : durations) {
        Put32(sttsBody, count);
        Put32(sttsBody, delta);
    }
    t.stts = Full("stts", 0, 0, sttsBody);

    // stsz: one size per sample; a fragmented source offers no constant size to copy.
    std::string stszBody;
    Put32(stszBody, 0);
    Put32(stszBody, uint32_t(frag.runs.size()));
    for (const auto& run : frag.runs) Put32(stszBody, run.bytes);
    t.stsz = Full("stsz", 0, 0, stszBody);

    // stsc: full chunks hold kPerChunk samples, a short last chunk its remainder, so one
    // entry covers an exact split and two cover any other.
    const size_t full = frag.runs.size() / kPerChunk, remainder = frag.runs.size() % kPerChunk;
    t.chunks = full + (remainder != 0);
    std::string stscBody;
    Put32(stscBody, t.chunks == 1 || !remainder ? 1u : 2u);
    Put32(stscBody, 1);  // first chunk
    Put32(stscBody, uint32_t(t.chunks == 1 ? frag.runs.size() : kPerChunk));
    Put32(stscBody, 1);  // sample description index
    if (t.chunks != 1 && remainder) {
        Put32(stscBody, uint32_t(t.chunks));
        Put32(stscBody, uint32_t(remainder));
        Put32(stscBody, 1);
    }
    t.stsc = Full("stsc", 0, 0, stscBody);
    return t;
}

// The whole movie box. Durations grow to 64-bit headers only when they must; the edit
// list re-states the source's playable start and length (media time, movie duration).
std::string BuildMoov(const Mp4Fragments& frag, const SampleTables& tables,
                      const std::vector<uint64_t>& chunkStarts, bool wide, uint64_t dataStart,
                      uint64_t skipOut, uint64_t editDuration, uint64_t movieDuration) {
    std::string mvhdBody;
    if (movieDuration > UINT32_MAX) {
        Put64(mvhdBody, 0); Put64(mvhdBody, 0); Put32(mvhdBody, frag.movieScale);
        Put64(mvhdBody, movieDuration);
    } else {
        Put32(mvhdBody, 0); Put32(mvhdBody, 0); Put32(mvhdBody, frag.movieScale);
        Put32(mvhdBody, uint32_t(movieDuration));
    }
    Put32(mvhdBody, 0x00010000);  // preferred rate, 1.0
    Put16(mvhdBody, 0x0100);      // full volume
    Put16(mvhdBody, 0);
    Put32(mvhdBody, 0); Put32(mvhdBody, 0);
    mvhdBody += Matrix();
    mvhdBody.append(24, '\0');
    Put32(mvhdBody, 2);  // next track id
    std::string moov = Full("mvhd", movieDuration > UINT32_MAX ? 1 : 0, 0, mvhdBody);

    std::string tkhdBody;
    if (movieDuration > UINT32_MAX) {
        Put64(tkhdBody, 0); Put64(tkhdBody, 0); Put32(tkhdBody, 1); Put32(tkhdBody, 0);
        Put64(tkhdBody, movieDuration);
    } else {
        Put32(tkhdBody, 0); Put32(tkhdBody, 0); Put32(tkhdBody, 1); Put32(tkhdBody, 0);
        Put32(tkhdBody, uint32_t(movieDuration));
    }
    tkhdBody.append(8, '\0');
    Put16(tkhdBody, 0);      // layer
    Put16(tkhdBody, 0);      // alternate group
    Put16(tkhdBody, 0x0100);  // this track's volume
    Put16(tkhdBody, 0);
    tkhdBody += Matrix();
    Put32(tkhdBody, 0); Put32(tkhdBody, 0);  // width, height
    std::string trak = Full("tkhd", movieDuration > UINT32_MAX ? 1 : 0, 3, tkhdBody);

    // The edit list: leading silence as the source stated it, then the playable window,
    // which begins where the first fragment sat relative to the original playable start.
    if (frag.hasEdit || frag.emptyEditDuration) {
        const uint32_t entries = uint32_t(frag.emptyEditDuration ? 1 : 0) + (frag.hasEdit ? 1 : 0);
        const bool wideElst = frag.emptyEditDuration > UINT32_MAX || editDuration > UINT32_MAX ||
                              skipOut > INT32_MAX;
        std::string elstBody;
        Put32(elstBody, entries);
        if (frag.emptyEditDuration) {
            if (wideElst) { Put64(elstBody, frag.emptyEditDuration); Put64(elstBody, ~(uint64_t)0); }
            else { Put32(elstBody, uint32_t(frag.emptyEditDuration)); Put32(elstBody, 0xFFFFFFFF); }
            Put32(elstBody, 0x00010000);
        }
        if (frag.hasEdit) {
            if (wideElst) { Put64(elstBody, editDuration); Put64(elstBody, skipOut); }
            else { Put32(elstBody, uint32_t(editDuration)); Put32(elstBody, uint32_t(skipOut)); }
            Put32(elstBody, 0x00010000);
        }
        trak += Box("edts", Full("elst", wideElst ? 1 : 0, 0, elstBody));
    }

    std::string mdhdBody;
    if (frag.mediaDuration > UINT32_MAX) {
        Put64(mdhdBody, 0); Put64(mdhdBody, 0); Put32(mdhdBody, frag.mediaScale);
        Put64(mdhdBody, frag.mediaDuration);
    } else {
        Put32(mdhdBody, 0); Put32(mdhdBody, 0); Put32(mdhdBody, frag.mediaScale);
        Put32(mdhdBody, uint32_t(frag.mediaDuration));
    }
    Put16(mdhdBody, 0x55C4);  // 'und'
    Put16(mdhdBody, 0);
    std::string mdia = Full("mdhd", frag.mediaDuration > UINT32_MAX ? 1 : 0, 0, mdhdBody);

    std::string hdlrBody;
    Put32(hdlrBody, 0);  // pre_defined
    hdlrBody.append("soun", 4);
    hdlrBody.append(12, '\0');
    hdlrBody.append("SoundHandler", 12);
    hdlrBody.push_back('\0');
    mdia += Full("hdlr", 0, 0, hdlrBody);

    std::string drefBody;
    Put32(drefBody, 1);
    drefBody += Full("url ", 0, 1, "");  // the samples live in this file
    std::string minf = Full("smhd", 0, 0, std::string(4, '\0')) + Box("dinf", Full("dref", 0, 0, drefBody));

    std::string stcoBody;
    Put32(stcoBody, uint32_t(tables.chunks));
    for (uint64_t at : chunkStarts) {
        if (wide) Put64(stcoBody, dataStart + at);
        else Put32(stcoBody, uint32_t(dataStart + at));
    }
    minf += Box("stbl", tables.stsd + tables.stts + tables.stsc + tables.stsz +
                          Full(wide ? "co64" : "stco", 0, 0, stcoBody));
    mdia += Box("minf", minf);
    trak += Box("mdia", mdia);
    moov += Box("trak", trak);
    return Box("moov", moov);
}

}  // namespace

bool RemuxFragmentedMp4(const std::wstring& from, const std::wstring& to, std::wstring& error) {
    Mp4Fragments frag;
    if (!ReadMp4Fragments(from, frag, error)) return false;
    // The sample table restarts at zero, so the fragment lead moves into the edit list's
    // playable start: the timeline a player sees stays the same, sample for sample.
    const uint64_t skip = frag.hasEdit ? (uint64_t)frag.editMediaTime : 0;
    const uint64_t skipOut = skip - frag.lead;  // ReadMp4Fragments checked skip >= lead
    const uint64_t total = frag.mediaDuration;
    if (skipOut >= total) {
        error = L"the track's playable window is empty";
        return false;
    }
    uint64_t playMovie = 0;
    if (!ScaleUp(total - skipOut, frag.mediaScale, frag.movieScale, true, playMovie)) {
        error = L"the track is too long for the rewrite to retime";
        return false;
    }
    // A length the source stated is copied as it is, so the file's exact end is kept; a
    // computed one only has to never clip the last sample.
    const uint64_t editDuration = frag.hasEdit && frag.editDuration ? frag.editDuration : playMovie;
    uint64_t movieDuration = 0;
    if (frag.hasEdit) movieDuration = frag.emptyEditDuration + editDuration;
    else if (!ScaleUp(total, frag.mediaScale, frag.movieScale, false, movieDuration)) {
        error = L"the track is too long for the rewrite to retime";
        return false;
    }

    // One offset per chunk, relative to where the mdat's data begins.
    const size_t chunks = (frag.runs.size() + kPerChunk - 1) / kPerChunk;
    std::vector<uint64_t> chunkStarts;
    chunkStarts.reserve(chunks);
    uint64_t dataBytes = 0;
    for (size_t c = 0; c < chunks; ++c) {
        chunkStarts.push_back(dataBytes);
        const size_t end = std::min(frag.runs.size(), (c + 1) * kPerChunk);
        for (size_t i = c * kPerChunk; i < end; ++i) dataBytes += frag.runs[i].bytes;
    }

    // The brand is the source's own; a stream that arrived without one gets a plain
    // audio-only identity rather than a copied movie brand.
    std::string ftyp(reinterpret_cast<const char*>(frag.ftyp.data()), frag.ftyp.size());
    if (ftyp.empty()) {
        std::string brands;
        brands.append("M4A ", 4);
        Put32(brands, 0);
        brands.append("M4A mp42isom", 12);
        ftyp = Box("ftyp", brands);
    }

    // The movie's size settles before its chunk offsets can be written, and only those
    // offsets' width (stco or co64, a 64-bit mdat) can change it: build once to measure,
    // then again with the real position.
    const SampleTables tables = BuildTables(frag);
    const auto moovOf = [&](bool wide) {
        return BuildMoov(frag, tables, chunkStarts, wide, 0, skipOut, editDuration, movieDuration);
    };
    std::string moov = moovOf(false);
    const bool wide = ftyp.size() + moov.size() + 8 + dataBytes > UINT32_MAX ||
                      8 + dataBytes > UINT32_MAX;
    if (wide) moov = moovOf(true);
    const size_t mdatHead = wide ? 16 : 8;
    const uint64_t dataStart = ftyp.size() + moov.size() + mdatHead;
    moov = BuildMoov(frag, tables, chunkStarts, wide, dataStart, skipOut, editDuration, movieDuration);

    std::string mdat;
    if (wide) {
        Put32(mdat, 1);
        mdat.append("mdat", 4);
        Put64(mdat, mdatHead + dataBytes);
    } else {
        Put32(mdat, uint32_t(mdatHead + dataBytes));
        mdat.append("mdat", 4);
    }

    File source;
    if (!source.Open(from)) {
        error = L"the stream could not be read again";
        return false;
    }
    source.Sequential();
    HANDLE out = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        error = L"the rewritten file could not be created";
        return false;
    }
    bool ok = WriteAll(out, ftyp) && WriteAll(out, moov) && WriteAll(out, mdat);
    std::vector<uint8_t> buffer(256 * 1024);
    for (const Mp4Fragments::Run& run : frag.runs) {
        for (uint64_t at = 0; ok && at < run.bytes;) {
            const size_t want = (size_t)std::min<uint64_t>(run.bytes - at, buffer.size());
            const uint8_t* p = source.View(run.offset + at, want);
            ok = p && WriteAll(out, p, want);
            at += want;
        }
    }
    ok = ok && FlushFileBuffers(out);
    CloseHandle(out);
    if (!ok) {
        DeleteFileW(to.c_str());
        error = L"writing the rewritten file did not finish";
        return false;
    }

    // Read the rewrite back and prove it holds the same samples before anyone tags it.
    PacketFile check;
    if (!check.OpenMp4(to)) {
        DeleteFileW(to.c_str());
        error = L"the rewritten file did not read back as a valid M4A";
        return false;
    }
    if (check.file.Size() != dataStart + dataBytes) {
        DeleteFileW(to.c_str());
        error = L"the rewritten file is not the size it should be";
        return false;
    }
    bool same = check.Count() == frag.runs.size() && check.codec == frag.codec &&
                check.channels == frag.channels && check.rate == frag.rate &&
                check.config == frag.config && check.frames == total && check.skip == skipOut &&
                check.mp4TimeScale == frag.mediaScale && check.mp4MovieScale == frag.movieScale;
    for (size_t i = 0; same && i < frag.runs.size(); ++i) {
        const Packet p = check.At(i);
        same = p.bytes == frag.runs[i].bytes && p.frames == frag.runs[i].frames;
    }
    if (!same) {
        DeleteFileW(to.c_str());
        error = L"the rewritten file's samples did not match the stream";
        return false;
    }
    return true;
}

}  // namespace nativeaudio
