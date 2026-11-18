// The locale data locale.cpp serves: glibc's compiled locale data and the Unicode fallback. The
// formats and their provenance are in locale_data.h.
//
// A name is looked up the way glibc's findlocale.c does it: when LOCPATH is set, only the
// directories it lists (then the system's own) are searched for a per-locale file; otherwise the
// archive is tried first and the directories after it. A per-locale file is found under the name
// forms intl/l10nflist.c generates, from the most specific (language_territory.codeset@modifier)
// down to the language alone. Aliases (locale.alias) are not read: a name that is only an alias
// gets the fallback, which is this layer's usual rule for a name it cannot load.
#include "locale_data.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The program's own allocator (malloc.cpp), declared here for the reason the rest of the layer
// does it: glibc's headers declare these names, and the definitions are cordinal's.
extern "C" void* malloc(size_t);
extern "C" void free(void*);

namespace locale_data {
namespace {

// glibc's category numbers (its <locale.h>), as the archive records and the lookups number them.
enum {
    kLCType = 0,
    kLCNumeric = 1,
    kLCTime = 2,
    kLCCollate = 3,
    kLCMonetary = 4,
    kLCMessages = 5,
    kLCAll = 6,
    kLCPaper = 7,
    kLCName = 8,
    kLCAddress = 9,
    kLCTelephone = 10,
    kLCMeasurement = 11,
    kLCIdentification = 12,
    kLCFirst = kLCType,
    kLCLast = kLCIdentification,
};

// The archive, as locale/locarchive.h defines it (AR_MAGIC, struct locarhead).
constexpr uint32_t kArchiveMagic = 0xde020109;
constexpr const char* kArchivePath = "/usr/lib/locale/locale-archive";

struct ArchiveHead {
    uint32_t magic;
    uint32_t serial;
    uint32_t namehashOffset;
    uint32_t namehashUsed;
    uint32_t namehashSize;
    uint32_t stringOffset;
    uint32_t stringUsed;
    uint32_t stringSize;
    uint32_t locrecOffset;
    uint32_t locrecUsed;
    uint32_t locrecSize;
    uint32_t sumhashOffset;
    uint32_t sumhashUsed;
    uint32_t sumhashSize;
};

struct NameHashEntry {
    uint32_t hashval;
    uint32_t nameOffset;
    uint32_t locrecOffset;
};

struct LocaleRecord {
    uint32_t refs;
    struct {
        uint32_t offset;
        uint32_t length;
    } record[13];
};

// The directories searched when no LOCPATH says otherwise, in order: glibc's own compiled-in path
// (COMPLOCALEDIR, which its findlocale.c falls back to at the end) and the older /usr/share form
// some distributions still unpack.
constexpr const char* kDefaultPaths[] = {"/usr/lib/locale", "/usr/share/locale"};
constexpr int kDefaultPathCount = (int)(sizeof kDefaultPaths / sizeof kDefaultPaths[0]);

// The category names as the per-locale files spell them (locale/categories.def).
const char* CategoryFileName(int category) {
    switch (category) {
        case kLCType: return "LC_CTYPE";
        case kLCNumeric: return "LC_NUMERIC";
        case kLCTime: return "LC_TIME";
        case kLCCollate: return "LC_COLLATE";
        case kLCMonetary: return "LC_MONETARY";
        case kLCMessages: return "LC_MESSAGES";
        case kLCPaper: return "LC_PAPER";
        case kLCName: return "LC_NAME";
        case kLCAddress: return "LC_ADDRESS";
        case kLCTelephone: return "LC_TELEPHONE";
        case kLCMeasurement: return "LC_MEASUREMENT";
        case kLCIdentification: return "LC_IDENTIFICATION";
        default: return nullptr;
    }
}

// Reads exactly `length` bytes at `offset`: a short read is retried, so the caller sees either all
// of the data or a failure.
bool ReadAt(int fd, uint64_t offset, void* out, size_t length) {
    char* at = (char*)out;
    while (length) {
        const ssize_t got = pread(fd, at, length, (off_t)offset);
        if (got <= 0) return false;
        at += got;
        offset += (uint64_t)got;
        length -= (size_t)got;
    }
    return true;
}

// A NUL-terminated string out of a file, for the archive's name table: `out` is always terminated,
// and a name that does not fit is a false answer (glibc's own names are short).
bool ReadName(int fd, uint32_t offset, char* out, size_t size) {
    for (size_t i = 0; i + 1 < size; ++i) {
        char c;
        if (!ReadAt(fd, (uint64_t)offset + i, &c, 1)) return false;
        out[i] = c;
        if (!c) return true;
    }
    out[size - 1] = 0;
    return false;
}

bool FileSize(int fd, uint64_t* size) {
    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size < 0) return false;
    *size = (uint64_t)info.st_size;
    return true;
}

// glibc's compute_hashval (locale/hashval.h): rotate nine bits in, then add the next byte. A hash
// of zero becomes all ones so that an empty name-table slot (offset zero) never matches.
uint32_t NameHash(const char* key, size_t length) {
    uint32_t value = (uint32_t)length;
    for (size_t i = 0; i < length; ++i) {
        value = (value << 9) | (value >> (32 - 9));
        value += (unsigned char)key[i];
    }
    return value ? value : ~0u;
}

// _nl_normalize_codeset (intl/l10nflist.c): the alphanumerics of a codeset with its letters
// lowercased, and "iso" in front when every character was a digit. localedef stores archive names
// with this form ("de_DE.UTF-8" is "de_DE.utf8" there), so the lookup needs it too.
bool NormalizeCodeset(const char* codeset, size_t length, char* out, size_t size) {
    size_t kept = 0;
    bool onlyDigits = true;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)codeset[i];
        const bool digit = c >= '0' && c <= '9';
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (digit || letter) {
            ++kept;
            if (!digit) onlyDigits = false;
        }
    }
    const size_t prefix = onlyDigits ? 3 : 0;
    if (prefix + kept + 1 > size) return false;
    char* at = out;
    if (onlyDigits) {
        memcpy(at, "iso", 3);
        at += 3;
    }
    for (size_t i = 0; i < length; ++i) {
        char c = codeset[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        const unsigned char byte = (unsigned char)c;
        const bool digit = byte >= '0' && byte <= '9';
        const bool letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        if (digit || letter) *at++ = c;
    }
    *at = 0;
    return true;
}

// The name the archive knows: the name with its codeset normalized, which is the only form
// localedef writes there. A name with no codeset is looked up as it stands.
bool ArchiveName(const char* name, char* out, size_t size) {
    const char* dot = nullptr;
    for (const char* p = name; *p; ++p) {
        if (*p == '.') {
            dot = p;
            break;
        }
        if (*p == '@') break;
    }
    if (!dot || !dot[1] || dot[1] == '@') {
        const size_t length = strlen(name);
        if (length + 1 > size) return false;
        memcpy(out, name, length + 1);
        return true;
    }
    const char* end = dot + 1;
    while (*end && *end != '@') ++end;
    const size_t head = (size_t)(dot - name);
    char normalized[64];
    if (!NormalizeCodeset(dot + 1, (size_t)(end - dot - 1), normalized, sizeof normalized)) return false;
    const size_t tail = strlen(end);
    if (head + 1 + strlen(normalized) + tail + 1 > size) return false;
    memcpy(out, name, head);
    out[head] = '.';
    char* at = out + head + 1;
    const size_t normalizedLength = strlen(normalized);
    memcpy(at, normalized, normalizedLength);
    memcpy(at + normalizedLength, end, tail + 1);
    return true;
}

// One category of one locale, read from `fd` at `base`: the magic, the item count and the item
// offsets, which are the block's first words (_nl_intern_locale_data in locale/loadlocale.c).
bool OpenBlock(int fd, uint64_t base, size_t size, unsigned magic, Block* block) {
    if (size < 8) return false;
    uint32_t header[2];
    if (!ReadAt(fd, base, header, sizeof header)) return false;
    if (header[0] != magic || header[1] == 0 || header[1] > (uint32_t)kMaxItems) return false;
    if (8 + (size_t)header[1] * 4 > size) return false;
    if (!ReadAt(fd, base + 8, block->index, (size_t)header[1] * 4)) return false;
    block->magic = header[0];
    block->nstrings = header[1];
    block->size = size;
    block->base = base;
    block->fd = fd;
    return true;
}

// The archive lookup of loadarchive.c: the name's hash picks a slot, a probe walks the table, and
// the slot names the locale record whose `category` entry is the block.
bool OpenFromArchive(const char* name, int category, Block* block) {
    char archiveName[256];
    if (!ArchiveName(name, archiveName, sizeof archiveName)) return false;
    const int fd = open(kArchivePath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool opened = false;
    ArchiveHead head;
    if (ReadAt(fd, 0, &head, sizeof head) && head.magic == kArchiveMagic && head.namehashSize > 2 &&
        head.namehashSize < (1u << 20)) {
        const uint32_t hashval = NameHash(archiveName, strlen(archiveName));
        size_t index = hashval % head.namehashSize;
        const size_t increment = 1 + hashval % (head.namehashSize - 2);
        uint32_t locrecOffset = 0;
        for (size_t probe = 0; probe < head.namehashSize && !locrecOffset; ++probe) {
            NameHashEntry entry;
            if (!ReadAt(fd, (uint64_t)head.namehashOffset + index * sizeof entry, &entry, sizeof entry)) break;
            if (entry.nameOffset == 0) break;  // an empty slot ends the probe: the name is not here
            if (entry.hashval == hashval) {
                char stored[256];
                if (ReadName(fd, entry.nameOffset, stored, sizeof stored) && !strcmp(stored, archiveName))
                    locrecOffset = entry.locrecOffset;
            }
            index += increment;
            if (index >= head.namehashSize) index -= head.namehashSize;
        }
        if (locrecOffset) {
            LocaleRecord record;
            if (ReadAt(fd, locrecOffset, &record, sizeof record) && category >= kLCFirst &&
                category <= kLCLast) {
                const uint64_t offset = record.record[category].offset;
                const uint32_t length = record.record[category].length;
                if (length > 0) opened = OpenBlock(fd, offset, length, MagicFor(category), block);
            }
        }
    }
    if (!opened) close(fd);
    return opened;
}

// The four parts of an XPG locale name, as intl/explodename.c splits it: language, an optional
// territory, codeset and modifier. A name too long for these parts is not searched for.
struct NameParts {
    char language[32];
    char territory[32];
    char codeset[32];
    char modifier[32];
    unsigned mask = 0;  // the bits of the parts that were given
};

bool CopyPart(const char* from, size_t length, char* out, size_t size) {
    if (length + 1 > size) return false;
    memcpy(out, from, length);
    out[length] = 0;
    return true;
}

bool ExplodeName(const char* name, NameParts* parts) {
    const char* at = name;
    const char* end = at;
    while (*end && *end != '_' && *end != '.' && *end != '@') ++end;
    if (!CopyPart(at, (size_t)(end - at), parts->language, sizeof parts->language)) return false;
    at = end;
    if (*at == '_') {
        end = ++at;
        while (*end && *end != '.' && *end != '@') ++end;
        if (!CopyPart(at, (size_t)(end - at), parts->territory, sizeof parts->territory)) return false;
        parts->mask |= 2;
        at = end;
    }
    if (*at == '.') {
        end = ++at;
        while (*end && *end != '@') ++end;
        if (!CopyPart(at, (size_t)(end - at), parts->codeset, sizeof parts->codeset)) return false;
        parts->mask |= 1;
        at = end;
    }
    if (*at == '@') {
        end = ++at;
        while (*end) ++end;
        if (!CopyPart(at, (size_t)(end - at), parts->modifier, sizeof parts->modifier)) return false;
        parts->mask |= 4;
        at = end;
    }
    return *at == 0;
}

// One name form out of the parts: the fields the mask names, in the standard order.
void BuildName(const NameParts& parts, unsigned mask, char* out, size_t size) {
    char* at = out;
    char* const end = out + size - 1;
    for (const char* p = parts.language; *p && at < end; ++p) *at++ = *p;
    if ((mask & 2) && at < end) {
        *at++ = '_';
        for (const char* p = parts.territory; *p && at < end; ++p) *at++ = *p;
    }
    if ((mask & 1) && at < end) {
        *at++ = '.';
        for (const char* p = parts.codeset; *p && at < end; ++p) *at++ = *p;
    }
    if ((mask & 4) && at < end) {
        *at++ = '@';
        for (const char* p = parts.modifier; *p && at < end; ++p) *at++ = *p;
    }
    *at = 0;
}

// A per-locale file: <directory>/<name form>/LC_<category>. The block is the whole file.
bool OpenFromFile(const char* directory, const char* name, int category, Block* block) {
    const char* file = CategoryFileName(category);
    if (!file) return false;
    char path[512];
    const char* pieces[] = {directory, "/", name, "/", file};
    size_t used = 0;
    for (const char* piece : pieces) {
        const size_t length = strlen(piece);
        if (used + length + 1 > sizeof path) return false;
        memcpy(path + used, piece, length + 1);
        used += length;
    }
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    uint64_t size = 0;
    if (!FileSize(fd, &size) || size < 8 || size > 0xFFFFFFFFu ||
        !OpenBlock(fd, 0, (size_t)size, MagicFor(category), block)) {
        close(fd);
        return false;
    }
    return true;
}

// Asks each directory for each name form, the most specific first. The forms are the successors
// _nl_make_l10nflist generates: every subset of the parts present, in decreasing bit order, so a
// missing "de_DE.UTF-8" file still finds "de_DE" and then "de".
bool OpenFromFiles(const char* name, int category, Block* block) {
    NameParts parts;
    if (!ExplodeName(name, &parts)) return false;
    char form[160];
    // LOCPATH's directories first (findlocale.c searches them and then the compiled-in path), then
    // the system's own through kDefaultPaths.
    const char* locpath = getenv("LOCPATH");
    for (unsigned subset = parts.mask + 1; subset-- > 0;) {
        if ((subset & ~parts.mask) != 0) continue;
        BuildName(parts, subset, form, sizeof form);
        if (locpath && *locpath) {
            const char* at = locpath;
            while (*at) {
                const char* end = at;
                while (*end && *end != ':') ++end;
                char directory[256];
                if (CopyPart(at, (size_t)(end - at), directory, sizeof directory) && directory[0] &&
                    OpenFromFile(directory, form, category, block))
                    return true;
                at = *end ? end + 1 : end;
            }
        }
        for (int i = 0; i < kDefaultPathCount; ++i)
            if (OpenFromFile(kDefaultPaths[i], form, category, block)) return true;
    }
    return false;
}

}  // namespace

unsigned MagicFor(int category) {    // LIMAGIC() in locale/localeinfo.h.
    if (category == kLCCollate) return 0x20051014u ^ (unsigned)category;
    if (category == kLCType) return 0x20090720u ^ (unsigned)category;
    return 0x20031115u ^ (unsigned)category;
}

bool Open(const char* name, int category, Block* block) {
    // glibc keeps its own C locale inside the library and never looks for its data.
    if (!name || !*name || !strcmp(name, "C") || !strcmp(name, "POSIX")) return false;
    const char* locpath = getenv("LOCPATH");
    if (!locpath || !*locpath) {
        if (OpenFromArchive(name, category, block)) return true;
    }
    return OpenFromFiles(name, category, block);
}

bool ReadItem(Block* block, unsigned item) {
    if (!block || item >= block->nstrings) return false;
    if (block->item[item]) return true;
    const size_t start = block->index[item];
    size_t end = block->size;
    for (unsigned i = 0; i < block->nstrings; ++i) {
        const size_t other = block->index[i];
        if (other > start && other < end) end = other;
    }
    if (start >= end || end > block->size) return false;
    // One byte more than the item holds, so that the last item of a block still ends in a NUL: the
    // tables never read past their own length and strings always have one.
    char* copy = (char*)malloc(end - start + 1);
    if (!copy) return false;
    if (!ReadAt(block->fd, block->base + start, copy, end - start)) {
        free(copy);
        return false;
    }
    copy[end - start] = 0;
    block->item[item] = copy;
    return true;
}

void ReleaseFile(Block* block) {
    if (!block) return;
    if (block->fd >= 0) close(block->fd);
    block->fd = -1;
}

void Close(Block* block) {
    if (!block) return;
    for (int i = 0; i < kMaxItems; ++i) {
        free(block->item[i]);
        block->item[i] = nullptr;
    }
    if (block->fd >= 0) close(block->fd);
    block->fd = -1;
    block->nstrings = 0;
}

namespace {

// Looks up `wc` in one of glibc's three-level tables (wctype/wchar-lookup.h): the table's first
// words are its shifts and bounds, then the level-one offsets, then the level-two offsets and the
// words that hold the answer. The bit form's level-three words are the class bits.
unsigned BitLookup(const void* table, unsigned wc) {
    const uint32_t* words = (const uint32_t*)table;
    if (!table || (wc >> words[0]) >= words[1]) return 0;
    const uint32_t first = words[5 + (wc >> words[0])];
    if (!first) return 0;
    const uint32_t second = *(const uint32_t*)((const char*)table + first + ((wc >> words[2]) & words[3]) * 4);
    if (!second) return 0;
    const uint32_t third = *(const uint32_t*)((const char*)table + second + ((wc >> 5) & words[4]) * 4);
    return (third >> (wc & 0x1f)) & 1;
}

// The map form of the same table: the level-three words are the difference between the answer and
// the argument, so a code point the table leaves out maps to itself.
unsigned MapLookup(const void* table, unsigned wc) {
    const uint32_t* words = (const uint32_t*)table;
    if (!table || (wc >> words[0]) >= words[1]) return wc;
    const uint32_t first = words[5 + (wc >> words[0])];
    if (!first) return wc;
    const uint32_t second = *(const uint32_t*)((const char*)table + first + ((wc >> words[2]) & words[3]) * 4);
    if (!second) return wc;
    const int32_t delta = *(const int32_t*)((const char*)table + second + (wc & words[4]) * 4);
    return (unsigned)(wc + (uint32_t)delta);
}

// The fallback's runs: `starts` holds the first code point of each run, `count` entries, and a run
// lasts until the next start (or the end of the plane).
int RunIndex(const uint16_t* starts, int count, unsigned wc) {
    if (wc > 0xFFFF) return -1;
    int low = 0;
    int high = count - 1;
    while (low < high) {
        const int middle = (low + high + 1) / 2;
        if (starts[middle] <= wc) low = middle;
        else high = middle - 1;
    }
    return low;
}

// ---- the Unicode fallback tables ----------------------------------------------------------------
//
// Generated from glibc's i18n locale definition (glibc 2.44's localedata/locales/i18n_ctype, the
// file that C.UTF-8's LC_CTYPE is compiled from) by a script that reads the file's `upper`,
// `lower`, `alpha`, `digit`, `xdigit`, `space`, `print`, `graph`, `blank`, `cntrl` and `punct`
// sections and its `toupper`/`tolower` maps, with `alnum` as alpha or digit. Each set became runs of
// equal answer over U+0000..U+FFFF: `kClassStarts`/`kClassMasks` give the class bits of a code
// point (in glibc's own _ISbit numbering, so they are the bits the layer already uses), and the
// case tables give the difference to add to a code point, as glibc's own map tables do. Every run
// was checked against the tables glibc compiled from the same file for C.UTF-8: no class, lower or
// upper answer over the plane differs, which is what the libc check compares again at run time.
// Unicode's version is the one glibc 2.44 carries (17.0.0).
//@@FALLBACK-TABLES@@
constexpr int kClassRunCount = 2224;
const uint16_t kClassStarts[kClassRunCount] = {
    0, 9, 10, 14, 32, 33, 48, 58, 65, 71, 91, 97, 103, 123, 127, 160,
    170, 171, 181, 182, 186, 187, 192, 215, 216, 223, 247, 248, 256, 257, 258, 259,
    260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275,
    276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291,
    292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307,
    308, 309, 310, 311, 313, 314, 315, 316, 317, 318, 319, 320, 321, 322, 323, 324,
    325, 326, 327, 328, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341,
    342, 343, 344, 345, 346, 347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357,
    358, 359, 360, 361, 362, 363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373,
    374, 375, 376, 378, 379, 380, 381, 382, 385, 387, 388, 389, 390, 392, 393, 396,
    398, 402, 403, 405, 406, 409, 412, 414, 415, 417, 418, 419, 420, 421, 422, 424,
    425, 426, 428, 429, 430, 432, 433, 436, 437, 438, 439, 441, 443, 444, 445, 448,
    452, 453, 454, 455, 456, 457, 458, 459, 460, 461, 462, 463, 464, 465, 466, 467,
    468, 469, 470, 471, 472, 473, 474, 475, 476, 478, 479, 480, 481, 482, 483, 484,
    485, 486, 487, 488, 489, 490, 491, 492, 493, 494, 495, 497, 498, 499, 500, 501,
    502, 505, 506, 507, 508, 509, 510, 511, 512, 513, 514, 515, 516, 517, 518, 519,
    520, 521, 522, 523, 524, 525, 526, 527, 528, 529, 530, 531, 532, 533, 534, 535,
    536, 537, 538, 539, 540, 541, 542, 543, 544, 545, 546, 547, 548, 549, 550, 551,
    552, 553, 554, 555, 556, 557, 558, 559, 560, 561, 562, 563, 570, 572, 573, 575,
    577, 578, 579, 583, 584, 585, 586, 587, 588, 589, 590, 591, 660, 662, 697, 704,
    706, 710, 722, 736, 741, 748, 749, 750, 751, 837, 838, 867, 880, 881, 882, 883,
    884, 885, 886, 887, 888, 890, 894, 895, 896, 900, 902, 903, 904, 907, 908, 909,
    910, 912, 913, 930, 931, 940, 975, 976, 978, 981, 984, 985, 986, 987, 988, 989,
    990, 991, 992, 993, 994, 995, 996, 997, 998, 999, 1000, 1001, 1002, 1003, 1004, 1005,
    1006, 1007, 1012, 1013, 1014, 1015, 1016, 1017, 1019, 1021, 1072, 1120, 1121, 1122, 1123, 1124,
    1125, 1126, 1127, 1128, 1129, 1130, 1131, 1132, 1133, 1134, 1135, 1136, 1137, 1138, 1139, 1140,
    1141, 1142, 1143, 1144, 1145, 1146, 1147, 1148, 1149, 1150, 1151, 1152, 1153, 1154, 1162, 1163,
    1164, 1165, 1166, 1167, 1168, 1169, 1170, 1171, 1172, 1173, 1174, 1175, 1176, 1177, 1178, 1179,
    1180, 1181, 1182, 1183, 1184, 1185, 1186, 1187, 1188, 1189, 1190, 1191, 1192, 1193, 1194, 1195,
    1196, 1197, 1198, 1199, 1200, 1201, 1202, 1203, 1204, 1205, 1206, 1207, 1208, 1209, 1210, 1211,
    1212, 1213, 1214, 1215, 1216, 1218, 1219, 1220, 1221, 1222, 1223, 1224, 1225, 1226, 1227, 1228,
    1229, 1230, 1232, 1233, 1234, 1235, 1236, 1237, 1238, 1239, 1240, 1241, 1242, 1243, 1244, 1245,
    1246, 1247, 1248, 1249, 1250, 1251, 1252, 1253, 1254, 1255, 1256, 1257, 1258, 1259, 1260, 1261,
    1262, 1263, 1264, 1265, 1266, 1267, 1268, 1269, 1270, 1271, 1272, 1273, 1274, 1275, 1276, 1277,
    1278, 1279, 1280, 1281, 1282, 1283, 1284, 1285, 1286, 1287, 1288, 1289, 1290, 1291, 1292, 1293,
    1294, 1295, 1296, 1297, 1298, 1299, 1300, 1301, 1302, 1303, 1304, 1305, 1306, 1307, 1308, 1309,
    1310, 1311, 1312, 1313, 1314, 1315, 1316, 1317, 1318, 1319, 1320, 1321, 1322, 1323, 1324, 1325,
    1326, 1327, 1328, 1329, 1367, 1369, 1370, 1376, 1417, 1419, 1421, 1424, 1425, 1456, 1470, 1471,
    1472, 1473, 1475, 1476, 1478, 1479, 1480, 1488, 1515, 1519, 1523, 1525, 1536, 1552, 1563, 1568,
    1624, 1625, 1642, 1646, 1748, 1749, 1757, 1761, 1769, 1773, 1789, 1791, 1792, 1806, 1807, 1808,
    1856, 1867, 1869, 1970, 1984, 2027, 2036, 2038, 2042, 2043, 2045, 2048, 2072, 2074, 2093, 2094,
    2096, 2111, 2112, 2137, 2140, 2142, 2143, 2144, 2155, 2160, 2184, 2185, 2192, 2194, 2199, 2200,
    2208, 2250, 2260, 2272, 2275, 2282, 2288, 2364, 2365, 2381, 2382, 2385, 2389, 2404, 2406, 2416,
    2417, 2436, 2437, 2445, 2447, 2449, 2451, 2473, 2474, 2481, 2482, 2483, 2486, 2490, 2492, 2493,
    2501, 2503, 2505, 2507, 2509, 2510, 2511, 2519, 2520, 2524, 2526, 2527, 2532, 2534, 2546, 2556,
    2557, 2559, 2561, 2564, 2565, 2571, 2575, 2577, 2579, 2601, 2602, 2609, 2610, 2612, 2613, 2615,
    2616, 2618, 2620, 2621, 2622, 2627, 2631, 2633, 2635, 2637, 2638, 2641, 2642, 2649, 2653, 2654,
    2655, 2662, 2678, 2679, 2689, 2692, 2693, 2702, 2703, 2706, 2707, 2729, 2730, 2737, 2738, 2740,
    2741, 2746, 2748, 2749, 2758, 2759, 2762, 2763, 2765, 2766, 2768, 2769, 2784, 2788, 2790, 2800,
    2802, 2809, 2813, 2816, 2817, 2820, 2821, 2829, 2831, 2833, 2835, 2857, 2858, 2865, 2866, 2868,
    2869, 2874, 2876, 2877, 2885, 2887, 2889, 2891, 2893, 2894, 2901, 2902, 2904, 2908, 2910, 2911,
    2916, 2918, 2928, 2929, 2930, 2936, 2946, 2948, 2949, 2955, 2958, 2961, 2962, 2966, 2969, 2971,
    2972, 2973, 2974, 2976, 2979, 2981, 2984, 2987, 2990, 3002, 3006, 3011, 3014, 3017, 3018, 3021,
    3022, 3024, 3025, 3031, 3032, 3046, 3056, 3067, 3072, 3085, 3086, 3089, 3090, 3113, 3114, 3130,
    3132, 3133, 3141, 3142, 3145, 3146, 3149, 3150, 3157, 3159, 3160, 3163, 3164, 3166, 3168, 3172,
    3174, 3184, 3191, 3200, 3204, 3205, 3213, 3214, 3217, 3218, 3241, 3242, 3252, 3253, 3258, 3260,
    3261, 3269, 3270, 3273, 3274, 3277, 3278, 3285, 3287, 3292, 3295, 3296, 3300, 3302, 3312, 3313,
    3316, 3328, 3341, 3342, 3345, 3346, 3387, 3389, 3397, 3398, 3401, 3402, 3405, 3406, 3407, 3408,
    3412, 3416, 3423, 3428, 3430, 3440, 3450, 3456, 3457, 3460, 3461, 3479, 3482, 3506, 3507, 3516,
    3517, 3518, 3520, 3527, 3530, 3531, 3535, 3541, 3542, 3543, 3544, 3552, 3558, 3568, 3570, 3572,
    3573, 3585, 3643, 3647, 3648, 3655, 3661, 3662, 3664, 3674, 3676, 3713, 3715, 3716, 3717, 3718,
    3723, 3724, 3748, 3749, 3750, 3751, 3770, 3771, 3774, 3776, 3781, 3782, 3783, 3784, 3789, 3790,
    3791, 3792, 3802, 3804, 3808, 3840, 3841, 3872, 3882, 3904, 3912, 3913, 3949, 3953, 3972, 3976,
    3992, 3993, 4029, 4030, 4045, 4046, 4059, 4096, 4151, 4152, 4153, 4155, 4170, 4176, 4254, 4256,
    4294, 4295, 4296, 4301, 4302, 4304, 4347, 4348, 4352, 4681, 4682, 4686, 4688, 4695, 4696, 4697,
    4698, 4702, 4704, 4745, 4746, 4750, 4752, 4785, 4786, 4790, 4792, 4799, 4800, 4801, 4802, 4806,
    4808, 4823, 4824, 4881, 4882, 4886, 4888, 4955, 4957, 4989, 4992, 5008, 5018, 5024, 5110, 5112,
    5118, 5120, 5121, 5741, 5743, 5760, 5761, 5787, 5789, 5792, 5867, 5870, 5881, 5888, 5908, 5910,
    5919, 5940, 5943, 5952, 5972, 5984, 5997, 5998, 6001, 6002, 6004, 6016, 6068, 6070, 6089, 6103,
    6104, 6108, 6109, 6110, 6112, 6122, 6128, 6138, 6144, 6160, 6170, 6176, 6265, 6272, 6315, 6320,
    6390, 6400, 6431, 6432, 6444, 6448, 6457, 6460, 6464, 6465, 6468, 6470, 6510, 6512, 6517, 6528,
    6572, 6576, 6602, 6608, 6618, 6619, 6622, 6656, 6684, 6686, 6688, 6751, 6752, 6753, 6773, 6781,
    6783, 6784, 6794, 6800, 6810, 6816, 6823, 6824, 6830, 6832, 6847, 6849, 6860, 6863, 6878, 6880,
    6892, 6912, 6964, 6965, 6980, 6981, 6989, 6990, 6992, 7002, 7040, 7082, 7084, 7142, 7143, 7154,
    7156, 7164, 7168, 7223, 7224, 7227, 7232, 7242, 7245, 7294, 7296, 7305, 7306, 7307, 7312, 7355,
    7357, 7360, 7368, 7376, 7401, 7405, 7406, 7412, 7413, 7415, 7418, 7419, 7424, 7616, 7635, 7669,
    7680, 7681, 7682, 7683, 7684, 7685, 7686, 7687, 7688, 7689, 7690, 7691, 7692, 7693, 7694, 7695,
    7696, 7697, 7698, 7699, 7700, 7701, 7702, 7703, 7704, 7705, 7706, 7707, 7708, 7709, 7710, 7711,
    7712, 7713, 7714, 7715, 7716, 7717, 7718, 7719, 7720, 7721, 7722, 7723, 7724, 7725, 7726, 7727,
    7728, 7729, 7730, 7731, 7732, 7733, 7734, 7735, 7736, 7737, 7738, 7739, 7740, 7741, 7742, 7743,
    7744, 7745, 7746, 7747, 7748, 7749, 7750, 7751, 7752, 7753, 7754, 7755, 7756, 7757, 7758, 7759,
    7760, 7761, 7762, 7763, 7764, 7765, 7766, 7767, 7768, 7769, 7770, 7771, 7772, 7773, 7774, 7775,
    7776, 7777, 7778, 7779, 7780, 7781, 7782, 7783, 7784, 7785, 7786, 7787, 7788, 7789, 7790, 7791,
    7792, 7793, 7794, 7795, 7796, 7797, 7798, 7799, 7800, 7801, 7802, 7803, 7804, 7805, 7806, 7807,
    7808, 7809, 7810, 7811, 7812, 7813, 7814, 7815, 7816, 7817, 7818, 7819, 7820, 7821, 7822, 7823,
    7824, 7825, 7826, 7827, 7828, 7829, 7838, 7839, 7840, 7841, 7842, 7843, 7844, 7845, 7846, 7847,
    7848, 7849, 7850, 7851, 7852, 7853, 7854, 7855, 7856, 7857, 7858, 7859, 7860, 7861, 7862, 7863,
    7864, 7865, 7866, 7867, 7868, 7869, 7870, 7871, 7872, 7873, 7874, 7875, 7876, 7877, 7878, 7879,
    7880, 7881, 7882, 7883, 7884, 7885, 7886, 7887, 7888, 7889, 7890, 7891, 7892, 7893, 7894, 7895,
    7896, 7897, 7898, 7899, 7900, 7901, 7902, 7903, 7904, 7905, 7906, 7907, 7908, 7909, 7910, 7911,
    7912, 7913, 7914, 7915, 7916, 7917, 7918, 7919, 7920, 7921, 7922, 7923, 7924, 7925, 7926, 7927,
    7928, 7929, 7930, 7931, 7932, 7933, 7934, 7935, 7944, 7952, 7958, 7960, 7966, 7968, 7976, 7984,
    7992, 8000, 8006, 8008, 8014, 8016, 8024, 8025, 8026, 8027, 8028, 8029, 8030, 8031, 8032, 8040,
    8048, 8062, 8064, 8072, 8080, 8088, 8096, 8104, 8112, 8117, 8118, 8120, 8125, 8126, 8127, 8130,
    8133, 8134, 8136, 8141, 8144, 8148, 8150, 8152, 8156, 8157, 8160, 8168, 8173, 8176, 8178, 8181,
    8182, 8184, 8189, 8191, 8192, 8199, 8200, 8203, 8232, 8234, 8287, 8288, 8293, 8294, 8305, 8306,
    8308, 8319, 8320, 8335, 8336, 8349, 8352, 8386, 8400, 8433, 8448, 8450, 8451, 8455, 8456, 8458,
    8459, 8462, 8464, 8467, 8468, 8469, 8470, 8473, 8478, 8484, 8485, 8486, 8487, 8488, 8489, 8490,
    8494, 8495, 8496, 8500, 8501, 8505, 8506, 8508, 8510, 8512, 8517, 8518, 8522, 8526, 8527, 8544,
    8560, 8576, 8579, 8580, 8581, 8585, 8588, 8592, 9258, 9280, 9291, 9312, 9398, 9424, 9450, 11124,
    11126, 11264, 11312, 11360, 11361, 11362, 11365, 11367, 11368, 11369, 11370, 11371, 11372, 11373, 11377, 11378,
    11379, 11381, 11382, 11390, 11393, 11394, 11395, 11396, 11397, 11398, 11399, 11400, 11401, 11402, 11403, 11404,
    11405, 11406, 11407, 11408, 11409, 11410, 11411, 11412, 11413, 11414, 11415, 11416, 11417, 11418, 11419, 11420,
    11421, 11422, 11423, 11424, 11425, 11426, 11427, 11428, 11429, 11430, 11431, 11432, 11433, 11434, 11435, 11436,
    11437, 11438, 11439, 11440, 11441, 11442, 11443, 11444, 11445, 11446, 11447, 11448, 11449, 11450, 11451, 11452,
    11453, 11454, 11455, 11456, 11457, 11458, 11459, 11460, 11461, 11462, 11463, 11464, 11465, 11466, 11467, 11468,
    11469, 11470, 11471, 11472, 11473, 11474, 11475, 11476, 11477, 11478, 11479, 11480, 11481, 11482, 11483, 11484,
    11485, 11486, 11487, 11488, 11489, 11490, 11491, 11493, 11499, 11500, 11501, 11502, 11503, 11506, 11507, 11508,
    11513, 11520, 11558, 11559, 11560, 11565, 11566, 11568, 11624, 11631, 11632, 11633, 11647, 11648, 11671, 11680,
    11687, 11688, 11695, 11696, 11703, 11704, 11711, 11712, 11719, 11720, 11727, 11728, 11735, 11736, 11743, 11744,
    11776, 11823, 11824, 11870, 11904, 11930, 11931, 12020, 12032, 12246, 12272, 12288, 12289, 12293, 12296, 12321,
    12330, 12337, 12342, 12344, 12349, 12352, 12353, 12439, 12441, 12445, 12448, 12449, 12539, 12540, 12544, 12549,
    12592, 12593, 12687, 12688, 12704, 12736, 12774, 12783, 12784, 12800, 12831, 12832, 13312, 19904, 19968, 42125,
    42128, 42183, 42192, 42238, 42240, 42509, 42512, 42540, 42560, 42561, 42562, 42563, 42564, 42565, 42566, 42567,
    42568, 42569, 42570, 42571, 42572, 42573, 42574, 42575, 42576, 42577, 42578, 42579, 42580, 42581, 42582, 42583,
    42584, 42585, 42586, 42587, 42588, 42589, 42590, 42591, 42592, 42593, 42594, 42595, 42596, 42597, 42598, 42599,
    42600, 42601, 42602, 42603, 42604, 42605, 42606, 42607, 42612, 42620, 42623, 42624, 42625, 42626, 42627, 42628,
    42629, 42630, 42631, 42632, 42633, 42634, 42635, 42636, 42637, 42638, 42639, 42640, 42641, 42642, 42643, 42644,
    42645, 42646, 42647, 42648, 42649, 42650, 42651, 42654, 42736, 42744, 42752, 42775, 42784, 42786, 42787, 42788,
    42789, 42790, 42791, 42792, 42793, 42794, 42795, 42796, 42797, 42798, 42799, 42802, 42803, 42804, 42805, 42806,
    42807, 42808, 42809, 42810, 42811, 42812, 42813, 42814, 42815, 42816, 42817, 42818, 42819, 42820, 42821, 42822,
    42823, 42824, 42825, 42826, 42827, 42828, 42829, 42830, 42831, 42832, 42833, 42834, 42835, 42836, 42837, 42838,
    42839, 42840, 42841, 42842, 42843, 42844, 42845, 42846, 42847, 42848, 42849, 42850, 42851, 42852, 42853, 42854,
    42855, 42856, 42857, 42858, 42859, 42860, 42861, 42862, 42863, 42873, 42874, 42875, 42876, 42877, 42879, 42880,
    42881, 42882, 42883, 42884, 42885, 42886, 42887, 42888, 42889, 42891, 42892, 42893, 42894, 42895, 42896, 42897,
    42898, 42899, 42902, 42903, 42904, 42905, 42906, 42907, 42908, 42909, 42910, 42911, 42912, 42913, 42914, 42915,
    42916, 42917, 42918, 42919, 42920, 42921, 42922, 42927, 42928, 42933, 42934, 42935, 42936, 42937, 42938, 42939,
    42940, 42941, 42942, 42943, 42944, 42945, 42946, 42947, 42948, 42952, 42953, 42954, 42955, 42957, 42958, 42959,
    42960, 42961, 42962, 42963, 42964, 42965, 42966, 42967, 42968, 42969, 42970, 42971, 42972, 42973, 42993, 42997,
    42998, 42999, 43000, 43003, 43014, 43015, 43048, 43053, 43056, 43066, 43072, 43124, 43128, 43136, 43204, 43205,
    43206, 43214, 43216, 43226, 43232, 43250, 43256, 43259, 43260, 43261, 43307, 43312, 43347, 43348, 43359, 43360,
    43389, 43392, 43443, 43444, 43456, 43470, 43471, 43482, 43486, 43488, 43519, 43520, 43575, 43584, 43598, 43600,
    43610, 43612, 43616, 43639, 43642, 43711, 43712, 43713, 43714, 43715, 43739, 43742, 43744, 43760, 43762, 43766,
    43767, 43777, 43783, 43785, 43791, 43793, 43799, 43808, 43815, 43816, 43823, 43824, 43867, 43868, 43882, 43884,
    43888, 43968, 44011, 44014, 44016, 44026, 44032, 55204, 55216, 55239, 55243, 55292, 57344, 63744, 64110, 64112,
    64218, 64256, 64263, 64275, 64280, 64285, 64297, 64298, 64311, 64312, 64317, 64318, 64319, 64320, 64322, 64323,
    64325, 64326, 64434, 64467, 64830, 64848, 64912, 64914, 64968, 64976, 65008, 65020, 65050, 65056, 65107, 65108,
    65127, 65128, 65132, 65136, 65141, 65142, 65277, 65279, 65280, 65281, 65296, 65306, 65313, 65339, 65345, 65371,
    65382, 65471, 65474, 65480, 65482, 65488, 65490, 65496, 65498, 65501, 65504, 65511, 65512, 65519, 65529, 65534,
};
const uint16_t kClassMasks[kClassRunCount] = {
    0x2, 0x2003, 0x2002, 0x2, 0x6001, 0xc004, 0xd808, 0xc004, 0xd508, 0xc508, 0xc004, 0xd608,
    0xc608, 0xc004, 0x2, 0xc004, 0xc608, 0xc004, 0xc608, 0xc004, 0xc608, 0xc004, 0xc508, 0xc004,
    0xc508, 0xc608, 0xc004, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc408, 0xc508, 0xc608, 0xc408,
    0xc508, 0xc708, 0xc608, 0xc508, 0xc708, 0xc608, 0xc508, 0xc708, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc708, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc408, 0xc608, 0xc408, 0xc608, 0xc004, 0xc408, 0xc004, 0xc608,
    0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc608, 0xc004, 0xc408, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc408, 0xc004, 0xc508, 0xc608, 0x0, 0xc608, 0xc004, 0xc508, 0x0, 0xc004, 0xc508, 0xc004,
    0xc508, 0x0, 0xc508, 0x0, 0xc508, 0xc608, 0xc508, 0x0, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc004, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc004, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0x0, 0xc508, 0x0, 0xc408, 0xc004, 0xc608,
    0xc004, 0x0, 0xc004, 0x0, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408,
    0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0x0, 0xc004, 0xc408, 0xc004, 0xc408,
    0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408,
    0xc004, 0x0, 0xc004, 0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0xc408, 0xc004,
    0xc408, 0x0, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0x0, 0xc004, 0x0, 0xc408, 0xc004,
    0x0, 0xc004, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0xc408, 0xc004, 0x0, 0xc408, 0xc004,
    0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004,
    0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408,
    0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0xc408,
    0xc004, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc004, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0x0,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc408, 0x0, 0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0x0, 0xc408, 0x0,
    0xc408, 0x0, 0xc408, 0xc004, 0x0, 0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc004, 0xc408,
    0x0, 0xc408, 0x0, 0xc408, 0xc004, 0x0, 0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408,
    0x0, 0xc408, 0xc004, 0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc408,
    0x0, 0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc004, 0xc408, 0xc004, 0xc408, 0x0, 0xc408,
    0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc004, 0xc408, 0x0, 0xc408, 0x0,
    0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408,
    0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408,
    0xc004, 0xc408, 0xc004, 0x0, 0xc408, 0xc004, 0xc408, 0x0, 0xc408, 0xc004, 0xc408, 0x0,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc004, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004,
    0x0, 0xc408, 0x0, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0x0, 0xc408,
    0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0xc408,
    0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc004, 0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc408,
    0x0, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0xc408,
    0x0, 0xc408, 0x0, 0xc004, 0x0, 0xc004, 0x0, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408,
    0xc004, 0xc408, 0xc004, 0xc508, 0x0, 0xc508, 0x0, 0xc508, 0x0, 0xc608, 0xc004, 0xc608,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc004, 0x0, 0xc408, 0xc004,
    0x0, 0xc508, 0x0, 0xc608, 0x0, 0xc004, 0xc408, 0xc004, 0xc408, 0x6001, 0xc408, 0xc004,
    0x0, 0xc408, 0xc004, 0xc408, 0x0, 0xc408, 0xc004, 0x0, 0xc408, 0xc004, 0x0, 0xc408,
    0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408,
    0xc004, 0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc004, 0x0, 0xc004, 0xc408, 0x0, 0xc408,
    0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0x0,
    0xc004, 0x0, 0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408,
    0xc004, 0x0, 0xc004, 0xc408, 0x0, 0xc004, 0xc408, 0x0, 0xc004, 0xc408, 0xc004, 0x0,
    0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc004, 0xc408, 0xc004, 0x0, 0xc004, 0xc408, 0xc004,
    0xc408, 0xc004, 0x0, 0xc004, 0x0, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0x0, 0xc004,
    0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0x0, 0xc004, 0xc408, 0xc004,
    0x0, 0xc004, 0xc408, 0x0, 0xc408, 0xc004, 0xc608, 0xc508, 0xc608, 0x0, 0xc508, 0x0,
    0xc508, 0xc004, 0x0, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0x0,
    0xc608, 0xc004, 0xc408, 0xc004, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0x0, 0xc508, 0x0, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0x0, 0xc508,
    0x0, 0xc608, 0x0, 0xc508, 0x0, 0xc508, 0x0, 0xc508, 0x0, 0xc508, 0xc608, 0xc508,
    0xc608, 0x0, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0x0, 0xc608, 0xc508,
    0xc004, 0xc608, 0xc004, 0xc608, 0x0, 0xc608, 0xc508, 0xc004, 0xc608, 0x0, 0xc608, 0xc508,
    0x0, 0xc004, 0xc608, 0xc508, 0xc004, 0x0, 0xc608, 0x0, 0xc608, 0xc508, 0xc004, 0x0,
    0x6001, 0xc004, 0x6001, 0xc004, 0x2002, 0xc004, 0x6001, 0xc004, 0x0, 0xc004, 0xc608, 0x0,
    0xc004, 0xc608, 0xc004, 0x0, 0xc608, 0x0, 0xc004, 0x0, 0xc004, 0x0, 0xc004, 0xc508,
    0xc004, 0xc508, 0xc004, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc004, 0xc508, 0xc004, 0xc508,
    0xc004, 0xc508, 0xc004, 0xc508, 0xc004, 0xc508, 0xc004, 0xc508, 0xc004, 0xc608, 0xc508, 0xc608,
    0xc408, 0xc608, 0xc004, 0xc608, 0xc508, 0xc004, 0xc508, 0xc608, 0xc004, 0xc608, 0xc004, 0xc508,
    0xc608, 0xc408, 0xc508, 0xc608, 0xc408, 0xc004, 0x0, 0xc004, 0x0, 0xc004, 0x0, 0xc004,
    0xc508, 0xc608, 0xc004, 0x0, 0xc004, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc004, 0xc508, 0xc608, 0xc508, 0xc608, 0xc004, 0xc508, 0xc608, 0x0,
    0xc004, 0xc608, 0x0, 0xc608, 0x0, 0xc608, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0x0,
    0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408,
    0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0xc408, 0xc004, 0x0,
    0xc004, 0x0, 0xc004, 0x0, 0xc004, 0x0, 0xc004, 0x6001, 0xc004, 0xc408, 0xc004, 0xc408,
    0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc004, 0xc408, 0xc004, 0xc408,
    0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc004, 0xc408, 0xc004, 0x0, 0xc004,
    0xc408, 0xc004, 0x0, 0xc004, 0xc408, 0xc004, 0xc408, 0x0, 0xc004, 0x0, 0xc408, 0xc004,
    0xc408, 0xc004, 0xc408, 0x0, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc408,
    0xc004, 0x0, 0xc004, 0xc408, 0xc004, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508,
    0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc408,
    0xc004, 0xc508, 0xc608, 0xc508, 0xc608, 0xc408, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608,
    0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0xc608, 0xc508, 0x0, 0xc608, 0xc508,
    0xc608, 0xc408, 0xc608, 0xc408, 0xc004, 0xc408, 0xc004, 0x0, 0xc004, 0x0, 0xc408, 0xc004,
    0x0, 0xc408, 0xc004, 0xc408, 0x0, 0xc004, 0xc408, 0x0, 0xc004, 0xc408, 0xc004, 0xc408,
    0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0x0, 0xc004, 0xc408, 0x0, 0xc408, 0xc004, 0xc408,
    0xc004, 0x0, 0xc408, 0x0, 0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408,
    0x0, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0x0, 0xc408, 0xc004,
    0xc408, 0xc004, 0xc408, 0xc004, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408,
    0x0, 0xc408, 0x0, 0xc608, 0xc004, 0xc608, 0xc004, 0x0, 0xc608, 0xc408, 0xc004, 0x0,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc004, 0xc408, 0x0, 0xc408,
    0x0, 0xc608, 0x0, 0xc608, 0x0, 0xc408, 0xc004, 0xc408, 0x0, 0xc408, 0x0, 0xc408,
    0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408, 0xc004, 0xc408,
    0xc004, 0x0, 0xc408, 0xc004, 0x0, 0xc004, 0x0, 0xc004, 0x0, 0xc004, 0x0, 0xc408,
    0x0, 0xc408, 0x0, 0xc004, 0x0, 0xc004, 0xc408, 0xc004, 0xc508, 0xc004, 0xc608, 0xc004,
    0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc408, 0x0, 0xc004, 0x0,
    0xc004, 0x0, 0xc004, 0x0,
};
constexpr int kLowerRunCount = 1273;
const uint16_t kLowerStarts[kLowerRunCount] = {
    0, 65, 91, 192, 215, 216, 223, 256, 257, 258, 259, 260, 261, 262, 263, 264,
    265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276, 277, 278, 279, 280,
    281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291, 292, 293, 294, 295, 296,
    297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 313,
    314, 315, 316, 317, 318, 319, 320, 321, 322, 323, 324, 325, 326, 327, 328, 330,
    331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346,
    347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 362,
    363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373, 374, 375, 376, 377, 378,
    379, 380, 381, 382, 385, 386, 387, 388, 389, 390, 391, 392, 393, 395, 396, 398,
    399, 400, 401, 402, 403, 404, 405, 406, 407, 408, 409, 412, 413, 414, 415, 416,
    417, 418, 419, 420, 421, 422, 423, 424, 425, 426, 428, 429, 430, 431, 432, 433,
    435, 436, 437, 438, 439, 440, 441, 444, 445, 452, 453, 454, 455, 456, 457, 458,
    459, 460, 461, 462, 463, 464, 465, 466, 467, 468, 469, 470, 471, 472, 473, 474,
    475, 476, 478, 479, 480, 481, 482, 483, 484, 485, 486, 487, 488, 489, 490, 491,
    492, 493, 494, 495, 497, 498, 499, 500, 501, 502, 503, 504, 505, 506, 507, 508,
    509, 510, 511, 512, 513, 514, 515, 516, 517, 518, 519, 520, 521, 522, 523, 524,
    525, 526, 527, 528, 529, 530, 531, 532, 533, 534, 535, 536, 537, 538, 539, 540,
    541, 542, 543, 544, 545, 546, 547, 548, 549, 550, 551, 552, 553, 554, 555, 556,
    557, 558, 559, 560, 561, 562, 563, 570, 571, 572, 573, 574, 575, 577, 578, 579,
    580, 581, 582, 583, 584, 585, 586, 587, 588, 589, 590, 591, 880, 881, 882, 883,
    886, 887, 895, 896, 902, 903, 904, 907, 908, 909, 910, 912, 913, 930, 931, 940,
    975, 976, 984, 985, 986, 987, 988, 989, 990, 991, 992, 993, 994, 995, 996, 997,
    998, 999, 1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1012, 1013, 1015, 1016, 1017, 1018,
    1019, 1021, 1024, 1040, 1072, 1120, 1121, 1122, 1123, 1124, 1125, 1126, 1127, 1128, 1129, 1130,
    1131, 1132, 1133, 1134, 1135, 1136, 1137, 1138, 1139, 1140, 1141, 1142, 1143, 1144, 1145, 1146,
    1147, 1148, 1149, 1150, 1151, 1152, 1153, 1162, 1163, 1164, 1165, 1166, 1167, 1168, 1169, 1170,
    1171, 1172, 1173, 1174, 1175, 1176, 1177, 1178, 1179, 1180, 1181, 1182, 1183, 1184, 1185, 1186,
    1187, 1188, 1189, 1190, 1191, 1192, 1193, 1194, 1195, 1196, 1197, 1198, 1199, 1200, 1201, 1202,
    1203, 1204, 1205, 1206, 1207, 1208, 1209, 1210, 1211, 1212, 1213, 1214, 1215, 1216, 1217, 1218,
    1219, 1220, 1221, 1222, 1223, 1224, 1225, 1226, 1227, 1228, 1229, 1230, 1232, 1233, 1234, 1235,
    1236, 1237, 1238, 1239, 1240, 1241, 1242, 1243, 1244, 1245, 1246, 1247, 1248, 1249, 1250, 1251,
    1252, 1253, 1254, 1255, 1256, 1257, 1258, 1259, 1260, 1261, 1262, 1263, 1264, 1265, 1266, 1267,
    1268, 1269, 1270, 1271, 1272, 1273, 1274, 1275, 1276, 1277, 1278, 1279, 1280, 1281, 1282, 1283,
    1284, 1285, 1286, 1287, 1288, 1289, 1290, 1291, 1292, 1293, 1294, 1295, 1296, 1297, 1298, 1299,
    1300, 1301, 1302, 1303, 1304, 1305, 1306, 1307, 1308, 1309, 1310, 1311, 1312, 1313, 1314, 1315,
    1316, 1317, 1318, 1319, 1320, 1321, 1322, 1323, 1324, 1325, 1326, 1327, 1329, 1367, 4256, 4294,
    4295, 4296, 4301, 4302, 5024, 5104, 5110, 7305, 7306, 7312, 7355, 7357, 7360, 7680, 7681, 7682,
    7683, 7684, 7685, 7686, 7687, 7688, 7689, 7690, 7691, 7692, 7693, 7694, 7695, 7696, 7697, 7698,
    7699, 7700, 7701, 7702, 7703, 7704, 7705, 7706, 7707, 7708, 7709, 7710, 7711, 7712, 7713, 7714,
    7715, 7716, 7717, 7718, 7719, 7720, 7721, 7722, 7723, 7724, 7725, 7726, 7727, 7728, 7729, 7730,
    7731, 7732, 7733, 7734, 7735, 7736, 7737, 7738, 7739, 7740, 7741, 7742, 7743, 7744, 7745, 7746,
    7747, 7748, 7749, 7750, 7751, 7752, 7753, 7754, 7755, 7756, 7757, 7758, 7759, 7760, 7761, 7762,
    7763, 7764, 7765, 7766, 7767, 7768, 7769, 7770, 7771, 7772, 7773, 7774, 7775, 7776, 7777, 7778,
    7779, 7780, 7781, 7782, 7783, 7784, 7785, 7786, 7787, 7788, 7789, 7790, 7791, 7792, 7793, 7794,
    7795, 7796, 7797, 7798, 7799, 7800, 7801, 7802, 7803, 7804, 7805, 7806, 7807, 7808, 7809, 7810,
    7811, 7812, 7813, 7814, 7815, 7816, 7817, 7818, 7819, 7820, 7821, 7822, 7823, 7824, 7825, 7826,
    7827, 7828, 7829, 7838, 7839, 7840, 7841, 7842, 7843, 7844, 7845, 7846, 7847, 7848, 7849, 7850,
    7851, 7852, 7853, 7854, 7855, 7856, 7857, 7858, 7859, 7860, 7861, 7862, 7863, 7864, 7865, 7866,
    7867, 7868, 7869, 7870, 7871, 7872, 7873, 7874, 7875, 7876, 7877, 7878, 7879, 7880, 7881, 7882,
    7883, 7884, 7885, 7886, 7887, 7888, 7889, 7890, 7891, 7892, 7893, 7894, 7895, 7896, 7897, 7898,
    7899, 7900, 7901, 7902, 7903, 7904, 7905, 7906, 7907, 7908, 7909, 7910, 7911, 7912, 7913, 7914,
    7915, 7916, 7917, 7918, 7919, 7920, 7921, 7922, 7923, 7924, 7925, 7926, 7927, 7928, 7929, 7930,
    7931, 7932, 7933, 7934, 7935, 7944, 7952, 7960, 7966, 7976, 7984, 7992, 8000, 8008, 8014, 8025,
    8026, 8027, 8028, 8029, 8030, 8031, 8032, 8040, 8048, 8072, 8080, 8088, 8096, 8104, 8112, 8120,
    8122, 8124, 8125, 8136, 8140, 8141, 8152, 8154, 8156, 8168, 8170, 8172, 8173, 8184, 8186, 8188,
    8189, 8486, 8487, 8490, 8491, 8492, 8498, 8499, 8544, 8560, 8579, 8580, 9398, 9424, 11264, 11312,
    11360, 11361, 11362, 11363, 11364, 11365, 11367, 11368, 11369, 11370, 11371, 11372, 11373, 11374, 11375, 11376,
    11377, 11378, 11379, 11381, 11382, 11390, 11392, 11393, 11394, 11395, 11396, 11397, 11398, 11399, 11400, 11401,
    11402, 11403, 11404, 11405, 11406, 11407, 11408, 11409, 11410, 11411, 11412, 11413, 11414, 11415, 11416, 11417,
    11418, 11419, 11420, 11421, 11422, 11423, 11424, 11425, 11426, 11427, 11428, 11429, 11430, 11431, 11432, 11433,
    11434, 11435, 11436, 11437, 11438, 11439, 11440, 11441, 11442, 11443, 11444, 11445, 11446, 11447, 11448, 11449,
    11450, 11451, 11452, 11453, 11454, 11455, 11456, 11457, 11458, 11459, 11460, 11461, 11462, 11463, 11464, 11465,
    11466, 11467, 11468, 11469, 11470, 11471, 11472, 11473, 11474, 11475, 11476, 11477, 11478, 11479, 11480, 11481,
    11482, 11483, 11484, 11485, 11486, 11487, 11488, 11489, 11490, 11491, 11499, 11500, 11501, 11502, 11506, 11507,
    42560, 42561, 42562, 42563, 42564, 42565, 42566, 42567, 42568, 42569, 42570, 42571, 42572, 42573, 42574, 42575,
    42576, 42577, 42578, 42579, 42580, 42581, 42582, 42583, 42584, 42585, 42586, 42587, 42588, 42589, 42590, 42591,
    42592, 42593, 42594, 42595, 42596, 42597, 42598, 42599, 42600, 42601, 42602, 42603, 42604, 42605, 42624, 42625,
    42626, 42627, 42628, 42629, 42630, 42631, 42632, 42633, 42634, 42635, 42636, 42637, 42638, 42639, 42640, 42641,
    42642, 42643, 42644, 42645, 42646, 42647, 42648, 42649, 42650, 42651, 42786, 42787, 42788, 42789, 42790, 42791,
    42792, 42793, 42794, 42795, 42796, 42797, 42798, 42799, 42802, 42803, 42804, 42805, 42806, 42807, 42808, 42809,
    42810, 42811, 42812, 42813, 42814, 42815, 42816, 42817, 42818, 42819, 42820, 42821, 42822, 42823, 42824, 42825,
    42826, 42827, 42828, 42829, 42830, 42831, 42832, 42833, 42834, 42835, 42836, 42837, 42838, 42839, 42840, 42841,
    42842, 42843, 42844, 42845, 42846, 42847, 42848, 42849, 42850, 42851, 42852, 42853, 42854, 42855, 42856, 42857,
    42858, 42859, 42860, 42861, 42862, 42863, 42873, 42874, 42875, 42876, 42877, 42878, 42879, 42880, 42881, 42882,
    42883, 42884, 42885, 42886, 42887, 42891, 42892, 42893, 42894, 42896, 42897, 42898, 42899, 42902, 42903, 42904,
    42905, 42906, 42907, 42908, 42909, 42910, 42911, 42912, 42913, 42914, 42915, 42916, 42917, 42918, 42919, 42920,
    42921, 42922, 42923, 42924, 42925, 42926, 42927, 42928, 42929, 42930, 42931, 42932, 42933, 42934, 42935, 42936,
    42937, 42938, 42939, 42940, 42941, 42942, 42943, 42944, 42945, 42946, 42947, 42948, 42949, 42950, 42951, 42952,
    42953, 42954, 42955, 42956, 42957, 42958, 42959, 42960, 42961, 42962, 42963, 42964, 42965, 42966, 42967, 42968,
    42969, 42970, 42971, 42972, 42973, 42997, 42998, 65313, 65339,
};
const int32_t kLowerDeltas[kLowerRunCount] = {
    0, 32, 0, 32, 0, 32, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, -199, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, -121, 1, 0, 1, 0, 1, 0,
    210, 1, 0, 1, 0, 206, 1, 0, 205, 1, 0, 79,
    202, 203, 1, 0, 205, 207, 0, 211, 209, 1, 0, 211,
    213, 0, 214, 1, 0, 1, 0, 1, 0, 218, 1, 0,
    218, 0, 1, 0, 218, 1, 0, 217, 1, 0, 1, 0,
    219, 1, 0, 1, 0, 2, 1, 0, 2, 1, 0, 2,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    2, 1, 0, 1, 0, -97, -56, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, -130,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 10795, 1, 0, -163, 10792,
    0, 1, 0, -195, 69, 71, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 116, 0,
    38, 0, 37, 0, 64, 0, 63, 0, 32, 0, 32, 0,
    8, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, -60, 0, 1, 0, -7, 1, 0, -130, 80, 32,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 15, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 48, 0, 7264, 0,
    7264, 0, 7264, 0, 38864, 8, 0, 1, 0, -3008, 0, -3008,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, -7615, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, -8, 0, -8,
    0, -8, 0, -8, 0, -8, 0, -8, 0, -8, 0, -8,
    0, -8, 0, -8, 0, -8, 0, -8, 0, -8, 0, -8,
    -74, -9, 0, -86, -9, 0, -8, -100, 0, -8, -112, -7,
    0, -128, -126, -9, 0, -7517, 0, -8383, -8262, 0, 28, 0,
    16, 0, 1, 0, 26, 0, 48, 0, 1, 0, -10743, -3814,
    -10727, 0, 1, 0, 1, 0, 1, 0, -10780, -10749, -10783, -10782,
    0, 1, 0, 1, 0, -10815, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, -35332, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, -42280, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, -42308, -42319, -42315, -42305, -42308, 0, -42258,
    -42282, -42261, 928, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, -48, -42307, -35384, 1, 0,
    1, 0, -42343, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, -42561, 0, 1, 0, 32,
    0,
};
constexpr int kUpperRunCount = 1305;
const uint16_t kUpperStarts[kUpperRunCount] = {
    0, 97, 123, 181, 182, 224, 247, 248, 255, 256, 257, 258, 259, 260, 261, 262,
    263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276, 277, 278,
    279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291, 292, 293, 294,
    295, 296, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310,
    311, 312, 314, 315, 316, 317, 318, 319, 320, 321, 322, 323, 324, 325, 326, 327,
    328, 329, 331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344,
    345, 346, 347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360,
    361, 362, 363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373, 374, 375, 376,
    378, 379, 380, 381, 382, 383, 384, 385, 387, 388, 389, 390, 392, 393, 396, 397,
    402, 403, 405, 406, 409, 410, 411, 412, 414, 415, 417, 418, 419, 420, 421, 422,
    424, 425, 429, 430, 432, 433, 436, 437, 438, 439, 441, 442, 445, 446, 447, 448,
    453, 454, 455, 456, 457, 458, 459, 460, 461, 462, 463, 464, 465, 466, 467, 468,
    469, 470, 471, 472, 473, 474, 475, 476, 477, 478, 479, 480, 481, 482, 483, 484,
    485, 486, 487, 488, 489, 490, 491, 492, 493, 494, 495, 496, 498, 499, 500, 501,
    502, 505, 506, 507, 508, 509, 510, 511, 512, 513, 514, 515, 516, 517, 518, 519,
    520, 521, 522, 523, 524, 525, 526, 527, 528, 529, 530, 531, 532, 533, 534, 535,
    536, 537, 538, 539, 540, 541, 542, 543, 544, 547, 548, 549, 550, 551, 552, 553,
    554, 555, 556, 557, 558, 559, 560, 561, 562, 563, 564, 572, 573, 575, 577, 578,
    579, 583, 584, 585, 586, 587, 588, 589, 590, 591, 592, 593, 594, 595, 596, 597,
    598, 600, 601, 602, 603, 604, 605, 608, 609, 610, 611, 612, 613, 614, 615, 616,
    617, 618, 619, 620, 621, 623, 624, 625, 626, 627, 629, 630, 637, 638, 640, 641,
    642, 643, 644, 647, 648, 649, 650, 652, 653, 658, 659, 669, 670, 671, 837, 838,
    881, 882, 883, 884, 887, 888, 891, 894, 940, 941, 944, 945, 962, 963, 972, 973,
    975, 976, 977, 978, 981, 982, 983, 984, 985, 986, 987, 988, 989, 990, 991, 992,
    993, 994, 995, 996, 997, 998, 999, 1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008,
    1009, 1010, 1011, 1012, 1013, 1014, 1016, 1017, 1019, 1020, 1072, 1104, 1120, 1121, 1122, 1123,
    1124, 1125, 1126, 1127, 1128, 1129, 1130, 1131, 1132, 1133, 1134, 1135, 1136, 1137, 1138, 1139,
    1140, 1141, 1142, 1143, 1144, 1145, 1146, 1147, 1148, 1149, 1150, 1151, 1152, 1153, 1154, 1163,
    1164, 1165, 1166, 1167, 1168, 1169, 1170, 1171, 1172, 1173, 1174, 1175, 1176, 1177, 1178, 1179,
    1180, 1181, 1182, 1183, 1184, 1185, 1186, 1187, 1188, 1189, 1190, 1191, 1192, 1193, 1194, 1195,
    1196, 1197, 1198, 1199, 1200, 1201, 1202, 1203, 1204, 1205, 1206, 1207, 1208, 1209, 1210, 1211,
    1212, 1213, 1214, 1215, 1216, 1218, 1219, 1220, 1221, 1222, 1223, 1224, 1225, 1226, 1227, 1228,
    1229, 1230, 1231, 1232, 1233, 1234, 1235, 1236, 1237, 1238, 1239, 1240, 1241, 1242, 1243, 1244,
    1245, 1246, 1247, 1248, 1249, 1250, 1251, 1252, 1253, 1254, 1255, 1256, 1257, 1258, 1259, 1260,
    1261, 1262, 1263, 1264, 1265, 1266, 1267, 1268, 1269, 1270, 1271, 1272, 1273, 1274, 1275, 1276,
    1277, 1278, 1279, 1280, 1281, 1282, 1283, 1284, 1285, 1286, 1287, 1288, 1289, 1290, 1291, 1292,
    1293, 1294, 1295, 1296, 1297, 1298, 1299, 1300, 1301, 1302, 1303, 1304, 1305, 1306, 1307, 1308,
    1309, 1310, 1311, 1312, 1313, 1314, 1315, 1316, 1317, 1318, 1319, 1320, 1321, 1322, 1323, 1324,
    1325, 1326, 1327, 1328, 1377, 1415, 4304, 4347, 4349, 4352, 5112, 5118, 7296, 7297, 7298, 7299,
    7301, 7302, 7303, 7304, 7305, 7306, 7307, 7545, 7546, 7549, 7550, 7566, 7567, 7681, 7682, 7683,
    7684, 7685, 7686, 7687, 7688, 7689, 7690, 7691, 7692, 7693, 7694, 7695, 7696, 7697, 7698, 7699,
    7700, 7701, 7702, 7703, 7704, 7705, 7706, 7707, 7708, 7709, 7710, 7711, 7712, 7713, 7714, 7715,
    7716, 7717, 7718, 7719, 7720, 7721, 7722, 7723, 7724, 7725, 7726, 7727, 7728, 7729, 7730, 7731,
    7732, 7733, 7734, 7735, 7736, 7737, 7738, 7739, 7740, 7741, 7742, 7743, 7744, 7745, 7746, 7747,
    7748, 7749, 7750, 7751, 7752, 7753, 7754, 7755, 7756, 7757, 7758, 7759, 7760, 7761, 7762, 7763,
    7764, 7765, 7766, 7767, 7768, 7769, 7770, 7771, 7772, 7773, 7774, 7775, 7776, 7777, 7778, 7779,
    7780, 7781, 7782, 7783, 7784, 7785, 7786, 7787, 7788, 7789, 7790, 7791, 7792, 7793, 7794, 7795,
    7796, 7797, 7798, 7799, 7800, 7801, 7802, 7803, 7804, 7805, 7806, 7807, 7808, 7809, 7810, 7811,
    7812, 7813, 7814, 7815, 7816, 7817, 7818, 7819, 7820, 7821, 7822, 7823, 7824, 7825, 7826, 7827,
    7828, 7829, 7830, 7835, 7836, 7841, 7842, 7843, 7844, 7845, 7846, 7847, 7848, 7849, 7850, 7851,
    7852, 7853, 7854, 7855, 7856, 7857, 7858, 7859, 7860, 7861, 7862, 7863, 7864, 7865, 7866, 7867,
    7868, 7869, 7870, 7871, 7872, 7873, 7874, 7875, 7876, 7877, 7878, 7879, 7880, 7881, 7882, 7883,
    7884, 7885, 7886, 7887, 7888, 7889, 7890, 7891, 7892, 7893, 7894, 7895, 7896, 7897, 7898, 7899,
    7900, 7901, 7902, 7903, 7904, 7905, 7906, 7907, 7908, 7909, 7910, 7911, 7912, 7913, 7914, 7915,
    7916, 7917, 7918, 7919, 7920, 7921, 7922, 7923, 7924, 7925, 7926, 7927, 7928, 7929, 7930, 7931,
    7932, 7933, 7934, 7935, 7936, 7944, 7952, 7958, 7968, 7976, 7984, 7992, 8000, 8006, 8017, 8018,
    8019, 8020, 8021, 8022, 8023, 8024, 8032, 8040, 8048, 8050, 8054, 8056, 8058, 8060, 8062, 8064,
    8072, 8080, 8088, 8096, 8104, 8112, 8114, 8115, 8116, 8126, 8127, 8131, 8132, 8144, 8146, 8160,
    8162, 8165, 8166, 8179, 8180, 8526, 8527, 8560, 8576, 8580, 8581, 9424, 9450, 11312, 11360, 11361,
    11362, 11365, 11366, 11367, 11368, 11369, 11370, 11371, 11372, 11373, 11379, 11380, 11382, 11383, 11393, 11394,
    11395, 11396, 11397, 11398, 11399, 11400, 11401, 11402, 11403, 11404, 11405, 11406, 11407, 11408, 11409, 11410,
    11411, 11412, 11413, 11414, 11415, 11416, 11417, 11418, 11419, 11420, 11421, 11422, 11423, 11424, 11425, 11426,
    11427, 11428, 11429, 11430, 11431, 11432, 11433, 11434, 11435, 11436, 11437, 11438, 11439, 11440, 11441, 11442,
    11443, 11444, 11445, 11446, 11447, 11448, 11449, 11450, 11451, 11452, 11453, 11454, 11455, 11456, 11457, 11458,
    11459, 11460, 11461, 11462, 11463, 11464, 11465, 11466, 11467, 11468, 11469, 11470, 11471, 11472, 11473, 11474,
    11475, 11476, 11477, 11478, 11479, 11480, 11481, 11482, 11483, 11484, 11485, 11486, 11487, 11488, 11489, 11490,
    11491, 11492, 11500, 11501, 11502, 11503, 11507, 11508, 11520, 11558, 11559, 11560, 11565, 11566, 42561, 42562,
    42563, 42564, 42565, 42566, 42567, 42568, 42569, 42570, 42571, 42572, 42573, 42574, 42575, 42576, 42577, 42578,
    42579, 42580, 42581, 42582, 42583, 42584, 42585, 42586, 42587, 42588, 42589, 42590, 42591, 42592, 42593, 42594,
    42595, 42596, 42597, 42598, 42599, 42600, 42601, 42602, 42603, 42604, 42605, 42606, 42625, 42626, 42627, 42628,
    42629, 42630, 42631, 42632, 42633, 42634, 42635, 42636, 42637, 42638, 42639, 42640, 42641, 42642, 42643, 42644,
    42645, 42646, 42647, 42648, 42649, 42650, 42651, 42652, 42787, 42788, 42789, 42790, 42791, 42792, 42793, 42794,
    42795, 42796, 42797, 42798, 42799, 42800, 42803, 42804, 42805, 42806, 42807, 42808, 42809, 42810, 42811, 42812,
    42813, 42814, 42815, 42816, 42817, 42818, 42819, 42820, 42821, 42822, 42823, 42824, 42825, 42826, 42827, 42828,
    42829, 42830, 42831, 42832, 42833, 42834, 42835, 42836, 42837, 42838, 42839, 42840, 42841, 42842, 42843, 42844,
    42845, 42846, 42847, 42848, 42849, 42850, 42851, 42852, 42853, 42854, 42855, 42856, 42857, 42858, 42859, 42860,
    42861, 42862, 42863, 42864, 42874, 42875, 42876, 42877, 42879, 42880, 42881, 42882, 42883, 42884, 42885, 42886,
    42887, 42888, 42892, 42893, 42897, 42898, 42899, 42900, 42901, 42903, 42904, 42905, 42906, 42907, 42908, 42909,
    42910, 42911, 42912, 42913, 42914, 42915, 42916, 42917, 42918, 42919, 42920, 42921, 42922, 42933, 42934, 42935,
    42936, 42937, 42938, 42939, 42940, 42941, 42942, 42943, 42944, 42945, 42946, 42947, 42948, 42952, 42953, 42954,
    42955, 42957, 42958, 42959, 42960, 42961, 42962, 42963, 42964, 42965, 42966, 42967, 42968, 42969, 42970, 42971,
    42972, 42998, 42999, 43859, 43860, 43888, 43968, 65345, 65371,
};
const int32_t kUpperDeltas[kUpperRunCount] = {
    0, -32, 0, 743, 0, -32, 0, -32, 121, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -232, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, -300, 195, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, 97, 0, -1, 163, 42561, 0, 130, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, 56, 0, -1, -2, 0, -1,
    -2, 0, -1, -2, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, -79, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, -2, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, 10815, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 10783, 10780,
    10782, -210, -206, 0, -205, 0, -202, 0, -203, 42319, 0, -205,
    42315, 0, -207, 42343, 42280, 42308, 0, -209, -211, 42308, 10743, 42305,
    0, -211, 0, 10749, -213, 0, -214, 0, 10727, 0, -218, 0,
    42307, -218, 0, 42282, -218, -69, -217, -71, 0, -219, 0, 42261,
    42258, 0, 84, 0, -1, 0, -1, 0, -1, 0, 130, 0,
    -38, -37, 0, -32, -31, -32, -64, -63, 0, -62, -57, 0,
    -47, -54, -8, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, -86, -80, 7, -116, 0, -96, 0, -1, 0,
    -1, 0, -32, -80, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, -15, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -48, 0, 3008, 0, 3008, 0, -8, 0, -6254, -6253, -6244, -6242,
    -6243, -6236, -6181, 35266, 0, -1, 0, 35332, 0, 3814, 0, 35384,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -59, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 8, 0, 8, 0,
    8, 0, 8, 0, 8, 0, 8, 0, 8, 0, 8, 0,
    8, 0, 8, 0, 74, 86, 100, 128, 112, 126, 0, 8,
    0, 8, 0, 8, 0, 8, 0, 9, 0, -7205, 0, 9,
    0, 8, 0, 8, 0, 7, 0, 9, 0, -28, 0, -16,
    0, -1, 0, -26, 0, -48, 0, -1, 0, -10795, -10792, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -7264, 0, -7264, 0,
    -7264, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0,
    -1, 0, -1, 48, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -928, 0, -38864, 0, -32, 0,
};
//@@END-FALLBACK-TABLES@@

}  // namespace

bool ClassBit(const void* table, unsigned wc) { return BitLookup(table, wc) != 0; }

unsigned MapValue(const void* table, unsigned wc) { return MapLookup(table, wc); }

unsigned FallbackClassMask(unsigned wc) {
    const int index = RunIndex(kClassStarts, kClassRunCount, wc);
    return index < 0 ? 0 : kClassMasks[index];
}

unsigned FallbackFold(unsigned wc, bool upper) {
    const int index = upper ? RunIndex(kUpperStarts, kUpperRunCount, wc)
                            : RunIndex(kLowerStarts, kLowerRunCount, wc);
    if (index < 0) return wc;
    const int32_t delta = upper ? kUpperDeltas[index] : kLowerDeltas[index];
    return (unsigned)(wc + (uint32_t)delta);
}

}  // namespace locale_data
