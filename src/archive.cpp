// ZIP archives as folders of music (archive.h): finds the central directory (ZIP64 too), lists its
// entries and inflates one member into memory. Only the central directory and the one member are
// read, so listing a large pack is cheap. Entry names are UTF-8 when the archive says so and code
// page 437 otherwise, as PKWARE's note defines them.
#include "archive.h"

#include "inflate.h"
#include "library.h"
#include "streamfile.h"

#include <algorithm>

namespace {

constexpr uint64_t kMaxMember = 1ull << 30;

uint32_t Le16(const std::string& s, size_t at) { return (uint8_t)s[at] | (uint32_t)(uint8_t)s[at + 1] << 8; }
uint32_t Le32(const std::string& s, size_t at) { return Le16(s, at) | Le16(s, at + 2) << 16; }
uint64_t Le64(const std::string& s, size_t at) { return Le32(s, at) | (uint64_t)Le32(s, at + 4) << 32; }

struct Entry {
    std::wstring name;  // with this platform's separators
    uint64_t local = 0, packed = 0, size = 0;
    uint32_t crc = 0;
};

std::wstring EntryName(const std::string& raw, bool utf8) {
    if (utf8 || std::all_of(raw.begin(), raw.end(), [](char c) { return (unsigned char)c < 0x80; })) return FromUtf8(raw);
    std::wstring out(raw.size(), L'\0');
    int n = MultiByteToWideChar(437, 0, raw.data(), (int)raw.size(), out.data(), (int)out.size());
    if (n <= 0) {  // no code page 437 here: keep the bytes as Latin-1 rather than lose the entry
        for (size_t i = 0; i < raw.size(); ++i) out[i] = (unsigned char)raw[i];
        return out;
    }
    out.resize((size_t)n);
    return out;
}

// The archive's playable entries: files only, not encrypted, stored or deflated, with names that
// stay inside the archive. False when there is no readable central directory.
bool ReadDirectory(FileSource& src, std::vector<Entry>& out) {
    const uint64_t size = src.Size();
    if (size < 22) return false;
    const size_t tail = (size_t)std::min<uint64_t>(size, 65557);
    std::string end;
    if (!src.Read(size - tail, tail, end)) return false;
    size_t eocd = std::string::npos;
    for (size_t at = tail - 22;; --at) {
        if (Le32(end, at) == 0x06054B50) { eocd = at; break; }
        if (at == 0) break;
    }
    if (eocd == std::string::npos) return false;
    uint64_t count = Le16(end, eocd + 10), dirSize = Le32(end, eocd + 12), dir = Le32(end, eocd + 16);
    // ZIP64: the locator just before the end record points at the 64-bit end record.
    if (eocd >= 20 && Le32(end, eocd - 20) == 0x07064B50) {
        std::string rec;
        if (!src.Read(Le64(end, eocd - 12), 56, rec) || Le32(rec, 0) != 0x06064B50) return false;
        count = Le64(rec, 32);
        dirSize = Le64(rec, 40);
        dir = Le64(rec, 48);
    }
    if (dir > size || dirSize > size - dir || dirSize > (256u << 20)) return false;
    std::string cd;
    if (!src.Read(dir, (size_t)dirSize, cd)) return false;
    size_t at = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (at + 46 > cd.size() || Le32(cd, at) != 0x02014B50) return false;
        const uint32_t flags = Le16(cd, at + 8), method = Le16(cd, at + 10);
        Entry e;
        e.crc = Le32(cd, at + 16);
        e.packed = Le32(cd, at + 20);
        e.size = Le32(cd, at + 24);
        e.local = Le32(cd, at + 42);
        const size_t nameLen = Le16(cd, at + 28), extraLen = Le16(cd, at + 30), commentLen = Le16(cd, at + 32);
        const size_t next = at + 46 + nameLen + extraLen + commentLen;
        if (next > cd.size()) return false;
        // ZIP64 sizes and offset, in that order, for each field the entry marks as 0xFFFFFFFF.
        for (size_t x = at + 46 + nameLen, xend = x + extraLen; x + 4 <= xend;) {
            const size_t id = Le16(cd, x), len = Le16(cd, x + 2), body = x + 4;
            if (body + len > xend) break;
            if (id == 1) {
                size_t f = body;
                auto take = [&](uint64_t& v) {
                    if (v == 0xFFFFFFFF && f + 8 <= body + len) { v = Le64(cd, f); f += 8; }
                };
                take(e.size);
                take(e.packed);
                take(e.local);
            }
            x = body + len;
        }
        std::string raw = cd.substr(at + 46, nameLen);
        at = next;
        std::replace(raw.begin(), raw.end(), '\\', '/');
        if (raw.empty() || raw.back() == '/' || (flags & 1) || (method != 0 && method != 8)) continue;
        // No absolute names, no climbing out, and no '|', which would read as a song number.
        if (raw[0] == '/' || raw.find(':') != std::string::npos || raw.find('|') != std::string::npos ||
            ("/" + raw + "/").find("/../") != std::string::npos)
            continue;
        e.name = EntryName(raw, (flags & 0x800) != 0);
        if constexpr (kPathSep != L'/') std::replace(e.name.begin(), e.name.end(), L'/', kPathSep);
        if (method == 0 && e.packed != e.size) continue;
        out.push_back(std::move(e));
    }
    return true;
}

bool SameName(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

}  // namespace

bool IsArchiveFile(const std::wstring& path) { return ExtOf(path) == L"zip"; }

bool SplitArchivePath(const std::wstring& path, std::wstring& archive, std::wstring& member) {
    for (size_t sep = path.find_first_of(L"\\/"); sep != std::wstring::npos; sep = path.find_first_of(L"\\/", sep + 1)) {
        if (sep < 5 || sep + 1 >= path.size()) continue;
        const std::wstring prefix = path.substr(0, sep);
        if (!IsArchiveFile(prefix)) continue;
        const DWORD attrs = GetFileAttributesW(prefix.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        archive = prefix;
        member = path.substr(sep + 1);
        if constexpr (kPathSep != L'/') std::replace(member.begin(), member.end(), L'/', kPathSep);
        return true;
    }
    return false;
}

std::wstring ContainerOf(const std::wstring& path) {
    std::wstring archive, member;
    return SplitArchivePath(path, archive, member) ? archive : path;
}

bool ListArchive(const std::wstring& archive, std::vector<std::wstring>& members) {
    FileSource src(archive);
    std::vector<Entry> entries;
    if (!src.Valid() || !ReadDirectory(src, entries)) return false;
    for (const Entry& e : entries) members.push_back(archive + kPathSep + e.name);
    return true;
}

std::shared_ptr<const std::string> ReadArchiveMember(const std::wstring& path) {
    std::wstring archive, member;
    if (!SplitArchivePath(path, archive, member)) return nullptr;
    // A song number belongs to the decoder, not the archive.
    const size_t bar = member.rfind(L'|');
    if (bar != std::wstring::npos) member.resize(bar);
    FileSource src(archive);
    std::vector<Entry> entries;
    if (!src.Valid() || !ReadDirectory(src, entries)) return nullptr;
    auto e = std::find_if(entries.begin(), entries.end(), [&](const Entry& x) { return SameName(x.name, member); });
    if (e == entries.end() || e->size > kMaxMember || e->packed > kMaxMember) return nullptr;
    std::string local;
    if (!src.Read(e->local, 30, local) || Le32(local, 0) != 0x04034B50) return nullptr;
    const uint64_t data = e->local + 30 + Le16(local, 26) + Le16(local, 28);
    std::string packed;
    if (!src.Read(data, (size_t)e->packed, packed)) return nullptr;
    auto out = std::make_shared<std::string>();
    if (Le16(local, 8) == 0) *out = std::move(packed);
    else if (!Inflate((const unsigned char*)packed.data(), packed.size(), (size_t)e->size, *out)) return nullptr;
    if (out->size() != e->size || Crc32((const unsigned char*)out->data(), out->size()) != e->crc) return nullptr;
    return out;
}
