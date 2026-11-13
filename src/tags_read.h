// What the tag reader's files share (tags.cpp dispatches; tags_fields.cpp, tags_id3.cpp, tags_flacogg.cpp,
// tags_mp4.cpp and tags_riff.cpp each parse one family of containers): bounds-checked byte access, the
// random-access File, the sink for pictures, lyrics, loops and chapters, and each parser's entry point.
// Nothing outside the tag reader includes this; tags.h is the public face.
#pragma once
#include "tags.h"

#include "archive.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace tagread {

// --- bytes -----------------------------------------------------------------------------------------------
// Every read past the end gives 0, so a parser can index a short buffer without checking first.
using sv = std::string_view;
inline uint8_t U8(sv s, size_t i) { return i < s.size() ? (uint8_t)s[i] : 0; }
inline uint32_t BE16(sv s, size_t i) { return (uint32_t)U8(s, i) << 8 | U8(s, i + 1); }
inline uint32_t BE24(sv s, size_t i) { return BE16(s, i) << 8 | U8(s, i + 2); }
inline uint32_t BE32(sv s, size_t i) { return BE16(s, i) << 16 | BE16(s, i + 2); }
inline uint64_t BE64(sv s, size_t i) { return (uint64_t)BE32(s, i) << 32 | BE32(s, i + 4); }
inline uint32_t LE16(sv s, size_t i) { return U8(s, i) | (uint32_t)U8(s, i + 1) << 8; }
inline uint32_t LE32(sv s, size_t i) { return LE16(s, i) | LE16(s, i + 2) << 16; }
inline uint64_t LE64(sv s, size_t i) { return LE32(s, i) | (uint64_t)LE32(s, i + 4) << 32; }
inline uint32_t Syncsafe(sv s, size_t i) {
    return (U8(s, i) & 0x7F) << 21 | (U8(s, i + 1) & 0x7F) << 14 | (U8(s, i + 2) & 0x7F) << 7 | (U8(s, i + 3) & 0x7F);
}
inline sv Sub(sv s, size_t pos, size_t n = sv::npos) { return pos >= s.size() ? sv() : s.substr(pos, n); }
// Kilobits a second over the whole file, as most formats report it.
inline uint32_t Kbps(uint64_t bytes, double seconds) { return (uint32_t)(bytes * 8 / seconds / 1000 + 0.5); }

// Random-access reader over one file. Reads are bounds-checked against the real size and capped
// at 64 MB, so a corrupt length in a tag header can't become a huge allocation.
class File {
public:
    explicit File(const std::wstring& path) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER sz{};
        if (h_ != INVALID_HANDLE_VALUE && GetFileSizeEx(h_, &sz)) size_ = (uint64_t)sz.QuadPart;
        else if (h_ == INVALID_HANDLE_VALUE && (member_ = ReadArchiveMember(path))) size_ = member_->size();
    }
    ~File() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    bool Ok() const { return h_ != INVALID_HANDLE_VALUE || member_; }
    uint64_t Size() const { return size_; }
    std::string Read(uint64_t offset, size_t n) const {
        if (offset >= size_ || n > (64u << 20)) return {};
        n = (size_t)std::min<uint64_t>(n, size_ - offset);
        if (member_) return member_->substr((size_t)offset, n);
        std::string buf(n, '\0');
        OVERLAPPED ov{};
        ov.Offset = (DWORD)offset;
        ov.OffsetHigh = (DWORD)(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(h_, buf.data(), (DWORD)n, &got, &ov)) got = 0;
        buf.resize(got);
        return buf;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    std::shared_ptr<const std::string> member_;  // a song inside a ZIP (archive.h)
};

// Loop points as the file states them; ReadLoopPoints interprets them.
struct LoopTags {
    std::string start, end, length;     // Vorbis comment values (first of each)
    bool wav = false;                   // the first loop of a WAV smpl chunk
    int64_t wavStart = 0, wavEnd = 0;   // frames; wavEnd exclusive
};

// What a read collects besides TagData. Each pointer is null when the caller doesn't want that
// part, and parsers skip reading it: a scan never loads a cover it would throw away.
struct ArtSink {
    std::string* image = nullptr;
    std::string* lyrics = nullptr;
    LoopTags* loops = nullptr;
    std::map<int, Chapter>* chapters = nullptr;  // keyed by order
    int bestType = -1;
    bool WantsImage() const { return image && bestType != 3; }
    // Picks the picture to keep: the front cover (type 3) always wins; otherwise the first seen.
    void Offer(int type, sv data) {
        if (!WantsImage() || data.empty()) return;
        if (bestType < 0 || type == 3) {
            image->assign(data);
            bestType = type < 0 ? 0 : type;
        }
    }
};

// --- text and fields (tags_fields.cpp) ---------------------------------------------------------------
std::string Latin1ToUtf8(sv s);
std::string Utf16ToUtf8(sv s, bool bigEndian);
std::string Trim(std::string s);
std::string Upper(sv s);
std::string GenreFromId(int id);
std::string CleanGenre(std::string g);
void SetIfEmpty(std::string& field, const std::string& value);
void AppendValue(std::string& field, const std::string& value);
void ReadNumberPair(const std::string& value, uint32_t& number, uint32_t& total);
// A generic KEY=value (Vorbis comment / APE / MP4 freeform / ASF / TXXX), any case.
void ApplyField(TagData& t, sv key, const std::string& value, bool append, std::string* lyrics = nullptr);
int ParseFlacPicture(sv b, sv& image);
void ApplyComment(sv key, sv value, TagData& t, ArtSink& art);
void ParseVorbisComments(sv b, TagData& t, ArtSink& art);

// --- containers -------------------------------------------------------------------------------------
void ParseId3v2(sv tag, TagData& t, ArtSink& art);                                   // tags_id3.cpp
void ParseId3v2File(const File& file, uint64_t tagPos, TagData& t, ArtSink& art);
bool ReadMpeg(const File& f, TagData& t, ArtSink& art);
bool ReadMusepack(const File& f, TagData& t, ArtSink& art);
bool ReadFlac(const File& f, uint64_t pos, TagData& t, ArtSink& art);                // tags_flacogg.cpp
bool ReadCord(const File& f, TagData& t, ArtSink& art);
bool ReadOgg(const File& f, TagData& t, ArtSink& art);
bool IsAsf(sv head);                                                                 // tags_mp4.cpp
bool ReadMp4(const File& f, TagData& t, ArtSink& art);
bool ReadAsf(const File& f, TagData& t, ArtSink& art);
bool ReadRiff(const File& f, TagData& t, ArtSink& art);                              // tags_riff.cpp
bool ReadAiff(const File& f, TagData& t, ArtSink& art);
bool ReadAu(const File& f, TagData& t);
bool ReadCaf(const File& f, TagData& t);
bool ReadDsf(const File& f, TagData& t, ArtSink& art);
bool ReadDff(const File& f, TagData& t, ArtSink& art);
bool ReadMatroska(const File& f, TagData& t);

}  // namespace tagread
