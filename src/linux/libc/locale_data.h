// The locale data behind locale.cpp (stage 6 of replacing glibc's text functions): glibc's compiled
// locale data, read from the locale archive or from per-locale files, and the Unicode tables that
// stand in on a system that has neither. Only locale.cpp includes this header.
//
// Every format here is glibc's own, read from its sources (November 18, 2026), the way time.cpp's
// TZif reader is RFC 8536's:
//   locale/locarchive.h and localedef's locarchive.c   the archive's header, name hash table,
//                                                      string table and per-locale records
//   locale/loadlocale.c (_nl_intern_locale_data())     one category's block: a magic number, the
//                                                      item count, the items' offsets, the items
//   locale/loadarchive.c                               the archive lookup: the hash, the probe,
//                                                      and a record's offset and length
//   locale/programs/ld-ctype.c, locale/categories.def  which item holds which table, and that the
//                                                      wide class and case tables are stored in
//                                                      the three-level form
//   wctype/wchar-lookup.h                              that three-level lookup (and its map form)
//   locale/findlocale.c, intl/l10nflist.c, intl/explodename.c, locale/hashval.h
//                                                      the search order, the name forms and the
//                                                      name hash
// The fallback tables are generated from glibc's i18n locale definition; locale_data.cpp says how.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace locale_data {

// The largest number of items a block may hold. glibc's LC_TIME block has 159 for a full locale and
// LC_CTYPE 72 plus a table each for its classes and maps (14 and 3 for i18n, so 89).
constexpr int kMaxItems = 192;

// One category's compiled data, opened from an archive record or a file. Only the items this layer
// asks for are read: a locale's block also carries transliteration, width and collation tables
// (most of its bytes) that nothing here uses.
struct Block {
    unsigned magic = 0;
    unsigned nstrings = 0;
    size_t size = 0;        // the block's length in bytes
    uint64_t base = 0;      // where the block starts in `fd`
    int fd = -1;            // the open file the block reads from, -1 when closed
    uint32_t index[kMaxItems] = {};
    void* item[kMaxItems] = {};

    Block() = default;
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
};

// Opens the data of `category` for the locale `name`, which must be the name setlocale was given:
// the archive first, then the per-locale directories. `category` is glibc's LC_* number. Returns
// false when this system has no data for that name and category.
bool Open(const char* name, int category, Block* block);

// Reads one item of an opened block, with its bytes copied out (so the file can be closed). The
// block owns the copy until Close. An item the block does not have is a false answer.
bool ReadItem(Block* block, unsigned item);

// Closes the block's descriptor once its items are read: every item is a copy, so nothing needs the
// file afterwards. Close still releases the copies.
void ReleaseFile(Block* block);

// Releases every item ReadItem copied and the descriptor Open kept.
void Close(Block* block);

// The category's magic number, as LIMAGIC() in locale/localeinfo.h computes it. `category` beyond
// LC_MESSAGES still uses the plain form; only LC_CTYPE and LC_COLLATE differ.
unsigned MagicFor(int category);

// One of glibc's three-level tables (wctype/wchar-lookup.h) answering for one code point: `ClassBit`
// reads a class table and answers the bit, `MapValue` reads a case table and answers the mapping
// (the code point itself when the table leaves it out). Both take the table itself, which is the
// item ReadItem returned.
bool ClassBit(const void* table, unsigned wc);
unsigned MapValue(const void* table, unsigned wc);

// The running locale's narrow case map, indexed by a byte 0..255: glibc reads the same table in
// __strcasecmp_l, which folds each byte through __tolower_l. Defined in locale.cpp, where the
// current locale's tables live, and asked for once per string, not once per byte.
const int* NarrowToLowerTable();

// The embedded Unicode tables: the class bits (in glibc's own numbering, as <ctype.h>'s _ISbit
// makes them) of a code point, and its simple case mapping. A code point above U+FFFF is in no
// class and maps to itself: the tables cover the Basic Multilingual Plane, which is what a system
// with no locale data needs. The bits are the twelve C classes; the combining classes glibc's data
// also has are not there.
unsigned FallbackClassMask(unsigned wc);
unsigned FallbackFold(unsigned wc, bool upper);

}  // namespace locale_data
