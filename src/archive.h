// Music inside ZIP archives, as game-music packs are usually shipped. A member is addressed as if
// the archive were a folder: "D:\Packs\Zelda.zip\Overworld.spc", or "...\Zelda.zip\Disc 1\01.nsf|3"
// for one song of a multi-song rip. The path names no file on disk, so anything that deletes, moves
// or rewrites files fails on it harmlessly; only readers that know about archives (the decoders'
// file openers, the tag reader, game music) look inside. Stored and deflated entries are read,
// with ZIP64; encrypted entries and other methods are not listed. A collection export
// (collection.h) holds a library's own files, so it is never listed as a pack.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class FileSource;  // streamfile.h

// One entry of a ZIP's central directory as stored, with ZIP64 sizes and offset resolved.
struct ZipEntry {
    std::string name;   // the stored bytes: '/'-separated, UTF-8 when flags has 0x800
    uint64_t local = 0, packed = 0, size = 0;  // local header offset, stored and real sizes
    uint32_t crc = 0;
    uint32_t unixTime = 0;  // the extended timestamp's modification time (0x5455), 0 when absent
    uint16_t flags = 0, method = 0;
};
// Every entry of the central directory, in order. False when there is no readable directory.
bool ReadZipDirectory(FileSource& src, std::vector<ZipEntry>& out);
// The entry's name decoded: UTF-8 when the archive says so (or the name is ASCII), else code page 437.
std::wstring ZipEntryName(const ZipEntry& e);
// Where the entry's data starts, past its local header; 0 when that header isn't there.
uint64_t ZipDataStart(FileSource& src, const ZipEntry& e);
// The entry that marks a collection export (collection.h); an archive holding it is not a pack.
constexpr char kCollectionManifest[] = "cordinal-collection.txt";

// True for a path whose extension is .zip.
bool IsArchiveFile(const std::wstring& path);

// Splits a member path into the archive on disk and the member's name inside it, with this
// platform's separators. A "|N" song suffix stays on the member. False when no folder of the path
// is an existing .zip file.
bool SplitArchivePath(const std::wstring& path, std::wstring& archive, std::wstring& member);
inline bool IsArchiveMember(const std::wstring& path) {
    std::wstring archive, member;
    return SplitArchivePath(path, archive, member);
}

// The file on disk that holds a path: the archive for a member, else the path itself. For
// existence checks only; never delete or rewrite what it returns on a member's behalf.
std::wstring ContainerOf(const std::wstring& path);

// Every readable file entry's member path, in archive order. False if the archive can't be read.
bool ListArchive(const std::wstring& archive, std::vector<std::wstring>& members);

// A member's whole contents, checked against its CRC-32. Null if the path isn't a member, the
// member is damaged or encrypted, or it is larger than 1 GiB. A "|N" suffix is ignored.
std::shared_ptr<const std::string> ReadArchiveMember(const std::wstring& path);
