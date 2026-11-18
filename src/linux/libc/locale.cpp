// cordinal's own locale layer (stage 6 of replacing glibc's text functions): setlocale, localeconv,
// nl_langinfo and its _l forms, the locale objects libstdc++ builds (newlocale, duplocale,
// freelocale, uselocale), the character classes and case maps (narrow and wide), the multibyte
// conversions, the C locale's collation, and strftime with wcsftime.
//
// A locale name is served the system's own data: glibc's compiled locale data, read from
// /usr/lib/locale/locale-archive or from the per-locale files some distributions ship unpacked
// (locale_data.cpp holds both readers, and the formats' provenance). LC_TIME gives the month, day
// and am/pm names and the four format strings, so nl_langinfo, strftime and wcsftime answer in the
// locale's own words; LC_CTYPE gives the narrow class table and the two case maps libstdc++'s
// ctype<char> reads out of a locale object, and the wide class and case tables, so iswalpha,
// towlower, iswctype and wcscasecmp fold and classify as glibc's do — Turkish's dotted I included.
// A name with no data on the system (Alpine's musl-style images, or a name nobody compiled) gets
// the embedded Unicode tables: full BMP classification and simple case mapping, which is what
// glibc's C.UTF-8 serves, so a UTF-8 locale still folds text. The C locale answers as the standard
// says, and a name whose codeset is not UTF-8 still converts single bytes.
//
// What stays the C locale's is the rest of the categories: localeconv, its decimal point and money
// formats, and the langinfo items of LC_NUMERIC, LC_MONETARY and LC_MESSAGES. Era years (%E) and
// alternative digits (%O) are not implemented: glibc's data carries them (Japanese's eras and
// kanji digits), and a format with either modifier answers as if it had none. An 8-bit locale's
// wide formatting is narrowed through UTF-8, so a non-ASCII month name in such a locale comes out
// as the bytes the file holds. All three are the layer's limits and are written down in
// docs/linux-libc.md.
//
// strftime follows glibc's answers, which the libc check compares byte for byte: the flags - _ 0 ^ #
// and a width, the E and O modifiers (as above: no alternative forms), a conversion it does not
// know copied out as it stands, and a result that does not fit returning 0 after writing what
// fitted. %s is mktime over a copy of the tm, so it normalises fields and honours tm_isdst as
// glibc's does; %Z reads tm_zone (which cordinal's localtime_r fills) and otherwise the zone's name.
//
// None of glibc's headers is included: they declare these same names, some as macros that would
// rewrite the definitions below, and btowc and wctob have inline forms in <wchar.h>. What the
// headers own (struct lconv's layout, struct tm, mbstate_t, the LC_* and langinfo numbers) is
// written out here and pinned against them by the libc check.
#include "locale_data.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <new>

// The process-wide allocator (malloc.cpp), the environment (files.cpp) and the calendar
// (time.cpp), declared here for the same reason as the rest of the file.
extern "C" void* malloc(size_t);
extern "C" void free(void*);
extern "C" char* getenv(const char* name);
extern "C" __attribute__((noreturn)) void abort(void) noexcept;
extern "C" __attribute__((visibility("hidden"))) const char* cordinal_zone_name(int isdst);

typedef unsigned int wint_t;
typedef unsigned long wctype_t;

namespace {

// glibc's struct lconv as <locale.h> lays it out on x86-64: ten strings, then eight bytes, then the
// international ones (which sit at 88 and later because of the padding).
struct Lconv {
    char* decimal_point;
    char* thousands_sep;
    char* grouping;
    char* int_curr_symbol;
    char* currency_symbol;
    char* mon_decimal_point;
    char* mon_thousands_sep;
    char* mon_grouping;
    char* positive_sign;
    char* negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

// glibc's struct tm: the nine standard fields, then the GNU zone offset and name.
struct Tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char* tm_zone;
};

// time.cpp's mktime, which reads and fills that same layout (%s asks it for the epoch seconds).
extern "C" long mktime(Tm* tm);

// glibc's mbstate_t on x86-64: a count and a small union (see printf.cpp and scanf.cpp). A partial
// UTF-8 sequence keeps the bytes it still needs in `count` and its bits in `state.value`.
struct MbState {
    int count;
    union {
        unsigned int value;
        char bytes[4];
    } state;
};

// The categories, numbered as <locale.h> numbers them.
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
    kCategoryCount = 13,
};

// The langinfo items the program can ask for, numbered as <langinfo.h> numbers them: an item number
// is a category above LC_ALL plus an index within it.
enum {
    kCodeset = 14,
    kRadixChar = 0x10000,
    kThouSep = 0x10001,
    kCrncYstr = 0x4000F,
    kYesExpr = 0x50000,
    kNoExpr = 0x50001,
    kTimeItem = 0x20000,
    kAbDayItem = 0,
    kDayItem = 7,
    kAbMonItem = 14,
    kMonItem = 26,
    kAmItem = 38,
    kPmItem = 39,
    kDateTimeFormat = 40,
    kDateFormat = 41,
    kTimeFormat = 42,
    kTimeFormatAmPm = 43,
    kEraItem = 44,
    kEraYearItem = 45,
    kEraDateFormat = 46,
    kAltDigitsItem = 47,
    kEraDateTimeFormat = 48,
    kEraTimeFormat = 49,
    kTimeItemCount = 50,
};

// LC_CTYPE's items, whose numbers the layer reads out of a block (locale/langinfo.h numbers them):
// the narrow class table and case maps libstdc++ wants, the codeset, the size of a multibyte
// character, and where the class and map tables start. Everything after those offsets is one
// table per class or map, in the order the names list gives.
enum {
    kCtypeClassItem = 0,
    kCtypeToUpperItem = 1,
    kCtypeToLowerItem = 3,
    kCtypeClassNamesItem = 10,
    kCtypeMapNamesItem = 11,
    kCtypeWidthItem = 12,
    kCtypeMbCurMaxItem = 13,
    kCtypeCodesetItem = 14,
    kCtypeClassOffsetItem = 17,
    kCtypeMapOffsetItem = 18,
};

// A loaded LC_CTYPE carries one table per class (glibc's i18n definition has 14) and per map (3),
// so the two arrays are roomy enough for any locale data localedef writes.
constexpr int kMaxClassTables = 32;
constexpr int kMaxMapTables = 8;

// glibc's struct __locale_struct, which libstdc++ (built against glibc) reads directly: its
// ctype<char> facet takes the narrow class table and the two case maps out of it. Those three
// pointers are the ones this layer fills from the loaded LC_CTYPE, exactly as glibc's own objects
// do, so a uselocale'd object carries its locale's tables. What this layer adds follows glibc's
// struct: the codeset the object answers with, and the two records it reads its data from.
struct LocaleRecord;
struct LocaleObject {
    void* locales[13];
    const unsigned short* ctypeTable;
    const int* tolowerTable;
    const int* toupperTable;
    const char* names[13];
    char codeset[40];
    LocaleRecord* ctype;
    LocaleRecord* time;
};
// The offsets are the ones libstdc++ reads, so the assertions below pin the layout down: if a glibc
// struct __locale_struct ever moves a member, the build stops here instead of the program handing
// libstdc++ a pointer read from the wrong place.
static_assert(sizeof(LocaleObject) == 288, "glibc's struct __locale_struct, then this layer's fields");
static_assert(offsetof(LocaleObject, ctypeTable) == 104, "glibc's __locale_struct.__ctype_b");
static_assert(offsetof(LocaleObject, tolowerTable) == 112, "glibc's __locale_struct.__ctype_tolower");
static_assert(offsetof(LocaleObject, toupperTable) == 120, "glibc's __locale_struct.__ctype_toupper");
static_assert(offsetof(LocaleObject, names[0]) == 128, "glibc's __locale_struct.__names");

constexpr int kNameSize = 48;
// A category's name, constant-initialized so that setlocale works before any constructor runs.
char gNames[kCategoryCount][kNameSize] = {"C", "C", "C", "C", "C", "C", "C",
                                          "C", "C", "C", "C", "C", "C"};

void SetName(char* destination, size_t size, const char* name) {
    size_t length = strlen(name);
    if (length >= size) length = size - 1;
    memcpy(destination, name, length);
    destination[length] = 0;
}

// Whether a locale name asks for UTF-8: the codeset after '.' up to '@'.
bool IsUtf8Codeset(const char* name) {
    const char* codeset = name;
    for (const char* p = name; *p; ++p) {
        if (*p == '.') {
            codeset = p + 1;
            break;
        }
        if (*p == '@') break;
    }
    const char* end = codeset;
    while (*end && *end != '@') ++end;
    const size_t length = (size_t)(end - codeset);
    const char* lower = length == 5 ? "utf-8" : length == 4 ? "utf8" : nullptr;
    if (!lower) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = codeset[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        if (c != lower[i]) return false;
    }
    return true;
}

// The environment variable a category reads when setlocale is asked for "".
const char* CategoryVariable(int category) {
    switch (category) {
        case kLCType: return "LC_CTYPE";
        case kLCNumeric: return "LC_NUMERIC";
        case kLCTime: return "LC_TIME";
        case kLCCollate: return "LC_COLLATE";
        case kLCMonetary: return "LC_MONETARY";
        case kLCMessages: return "LC_MESSAGES";
        default: return nullptr;
    }
}

// glibc's order for "": LC_ALL, then the category's own variable, then LANG; "C" when none is set.
const char* ResolveName(int category) {
    if (const char* all = getenv("LC_ALL"); all && *all) return all;
    if (const char* own = CategoryVariable(category); own) {
        if (const char* value = getenv(own); value && *value) return value;
    }
    if (const char* lang = getenv("LANG"); lang && *lang) return lang;
    return "C";
}

// Loads the data a category's name asks for (defined below, with the rest of the data layer).
void SelectRecord(int category, const char* name);

void StoreName(int category, const char* name) {
    if (category == kLCAll) {
        for (int i = 0; i < kCategoryCount; ++i)
            if (i != kLCAll) StoreName(i, name);
        return;
    }
    SetName(gNames[category], kNameSize, name);
    SelectRecord(category, gNames[category]);
}

// The name setlocale reports: the one name when every category shares it, else glibc's compound of
// every category in order (LC_ALL is the mask over them, not one of them).
char* NameOf(int category) {
    if (category != kLCAll) return gNames[category];
    bool same = true;
    for (int i = 0; i < kCategoryCount && same; ++i)
        if (i != kLCAll) same = !strcmp(gNames[i], gNames[kLCType]);
    if (same) return gNames[kLCType];
    static const char* const kNames[kCategoryCount] = {"LC_CTYPE",      "LC_NUMERIC",     "LC_TIME",
                                                      "LC_COLLATE",    "LC_MONETARY",    "LC_MESSAGES",
                                                      "LC_ALL",        "LC_PAPER",       "LC_NAME",
                                                      "LC_ADDRESS",    "LC_TELEPHONE",   "LC_MEASUREMENT",
                                                      "LC_IDENTIFICATION"};
    static char compound[kCategoryCount * (kNameSize + 20)];
    char* at = compound;
    for (int i = 0; i < kCategoryCount; ++i) {
        if (i == kLCAll) continue;
        if (at != compound) *at++ = ';';
        const char* name = kNames[i];
        while (*name) *at++ = *name++;
        *at++ = '=';
        for (const char* value = gNames[i]; *value; ++value) *at++ = *value;
    }
    *at = 0;
    return compound;
}

// ---- the C locale's data ----------------------------------------------------------------------

const char* const kAbDay[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kDay[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const kAbMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* const kMon[12] = {"January", "February", "March",     "April",   "May",      "June",
                              "July",    "August",   "September", "October", "November", "December"};

// Every LC_TIME item the layer serves, as the C locale has them: the names above and the four
// formats, with nothing in the era and alternative-digit items. An item a loaded block does not
// carry reads from here; a block that carries an empty string serves the empty string, which is
// how glibc reads it too (de_DE's am/pm strings are empty, so %p prints nothing there).
const char* const kCTimeItems[kTimeItemCount] = {
    kAbDay[0], kAbDay[1], kAbDay[2], kAbDay[3], kAbDay[4], kAbDay[5], kAbDay[6],
    kDay[0],   kDay[1],   kDay[2],   kDay[3],   kDay[4],   kDay[5],   kDay[6],
    kAbMon[0], kAbMon[1], kAbMon[2], kAbMon[3], kAbMon[4], kAbMon[5], kAbMon[6], kAbMon[7],
    kAbMon[8], kAbMon[9], kAbMon[10], kAbMon[11],
    kMon[0],   kMon[1],   kMon[2],   kMon[3],   kMon[4],   kMon[5],   kMon[6],   kMon[7],
    kMon[8],   kMon[9],   kMon[10],  kMon[11],
    "AM", "PM", "%a %b %e %H:%M:%S %Y", "%m/%d/%y", "%H:%M:%S", "%I:%M:%S %p",
    "", "", "", "", "", ""};

char kPoint[] = ".";
char kEmpty[] = "";
// glibc's C locale: '.' as the decimal point, nothing grouped, and CHAR_MAX wherever the locale does
// not say (which is what the standard asks for).
Lconv gLconv = {kPoint,  kEmpty,  kEmpty,  kEmpty,  kEmpty,  kEmpty,  kEmpty,  kEmpty,
                kEmpty,  kEmpty,  CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
                CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX};

// ---- the character classes --------------------------------------------------------------------

// The classes as glibc numbers them (the same values glibc's <ctype.h> _ISbit() gives and that
// libstdc++'s ctype_base::mask carries), because libstdc++ passes those bits to __iswctype_l.
enum {
    kBlank = 1 << 0,
    kCntrl = 1 << 1,
    kPunct = 1 << 2,
    kAlnum = 1 << 3,
    kUpper = 1 << 8,
    kLower = 1 << 9,
    kAlpha = 1 << 10,
    kDigit = 1 << 11,
    kXdigit = 1 << 12,
    kSpace = 1 << 13,
    kPrint = 1 << 14,
    kGraph = 1 << 15,
};

// The C locale's class for one ASCII byte. Every other byte is in no class, which is what glibc's C
// locale says, and what a UTF-8 locale says about the bytes above 0x7F that make up its characters.
constexpr unsigned Classify(int c) {
    if (c >= 'A' && c <= 'Z') return kUpper | kAlpha | kAlnum | kPrint | kGraph | (c <= 'F' ? kXdigit : 0);
    if (c >= 'a' && c <= 'z') return kLower | kAlpha | kAlnum | kPrint | kGraph | (c <= 'f' ? kXdigit : 0);
    if (c >= '0' && c <= '9') return kDigit | kAlnum | kXdigit | kPrint | kGraph;
    if (c == ' ') return kSpace | kBlank | kPrint;
    if (c == '\t') return kSpace | kBlank | kCntrl;
    if (c == '\n' || c == '\v' || c == '\f' || c == '\r') return kSpace | kCntrl;
    if (c < 0x20 || c == 0x7F) return kCntrl;
    if (c >= 0x21 && c <= 0x7E) return kPunct | kPrint | kGraph;
    return 0;
}

constexpr int LowerOf(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
constexpr int UpperOf(int c) { return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; }

// The byte a narrow ctype argument stands for: glibc's tables have a half for -128..-1 as well, and
// tolower and toupper hand back a value outside them unchanged. -1 is glibc's EOF slot, which is not
// the mirror of byte 255: it is in no class and never reaches a table here.
bool ByteOf(int c, int& byte) {
    if (c < -128 || c > 255 || c == -1) return false;
    byte = c < 0 ? c + 256 : c;
    return true;
}

// The twelve names wctype() knows, in a fixed order, and the class bit each one stands for. A loaded
// locale also names combining classes, which the layer serves through the data's own list; these
// twelve are the ones the C standard and <wctype.h> carry, and the fallback has.
enum {
    kWctypeAlnum = 0,
    kWctypeAlpha,
    kWctypeBlank,
    kWctypeCntrl,
    kWctypeDigit,
    kWctypeGraph,
    kWctypeLower,
    kWctypePrint,
    kWctypePunct,
    kWctypeSpace,
    kWctypeUpper,
    kWctypeXdigit,
    kWctypeCount,
};
const unsigned kCtypeBits[kWctypeCount] = {kAlnum, kAlpha, kBlank, kCntrl, kDigit, kGraph,
                                           kLower, kPrint, kPunct, kSpace, kUpper, kXdigit};
const char* const kWctypeNames[kWctypeCount] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                                "lower", "print", "punct", "space", "upper", "xdigit"};

// glibc's narrow ctype tables: 384 entries, indexed by the byte value plus 128, holding the class
// bits above (for the class table) or the folded byte (for the two case maps). libstdc++'s
// ctype<char> facet reads them straight from every locale object, so they are built before the first
// object is handed out. A loaded LC_CTYPE replaces them, per record, with its own.
unsigned short gCtypeTable[384];
int gToLowerTable[384];
int gToUpperTable[384];
bool gTablesReady = false;

// ---- the loaded locale data --------------------------------------------------------------------

// What a record's ctype answers from: the C locale's ASCII tables, the embedded Unicode tables (a
// UTF-8 name with no data on this system), or a loaded LC_CTYPE with its own narrow tables and its
// three-level class and map tables.
enum DataKind { kCData, kUnicodeData, kLoadedData };

struct CtypeTables {
    DataKind kind = kCData;
    const unsigned short* narrowClass = nullptr;  // 384 entries, the byte plus 128
    const int* narrowLower = nullptr;
    const int* narrowUpper = nullptr;
    const char* classNameList = nullptr;  // NUL-separated names, then an empty string
    const char* mapNameList = nullptr;
    const void* classTable[kMaxClassTables] = {};
    unsigned classCount = 0;
    const void* mapTable[kMaxMapTables] = {};
    unsigned mapCount = 0;
    // The data's own index for each of the twelve class names and for the two maps, -1 when its
    // list does not have that name.
    int classItem[kWctypeCount] = {};
    int lowerItem = -1;
    int upperItem = -1;
    const char* codeset = nullptr;  // the data's codeset name, when it has one
    unsigned mbCurMax = 1;
    bool utf8 = false;
};

struct TimeTables {
    bool loaded = false;
    const char* items[kTimeItemCount] = {};
};

// One locale name's data, loaded once and kept for the life of the program: glibc's own archive
// data is undeletable there too, and cordinal switches locales a handful of times at most.
struct LocaleRecord {
    LocaleRecord* next;
    char name[kNameSize];
    CtypeTables ctype;
    TimeTables time;
    locale_data::Block ctypeBlock;
    locale_data::Block timeBlock;
};

LocaleRecord gCRecord;               // the C locale's own: ASCII tables, English names
LocaleRecord* gRecords;              // every name looked up so far
LocaleRecord* gGlobal[kCategoryCount];  // the global locale's LC_CTYPE and LC_TIME records
thread_local void* threadLocale;     // the thread's own locale object, from uselocale

void SelectRecord(int category, const char* name);

LocaleRecord* Load(const char* name);

// Builds the C locale's narrow tables and its record (defined with the class functions below).
void EnsureTables();

LocaleRecord* CRecord() {
    EnsureTables();
    return &gCRecord;
}

const CtypeTables& CurrentCtype() {
    EnsureTables();
    if (void* locale = threadLocale) {
        const LocaleObject* object = (const LocaleObject*)locale;
        if (object->ctype) return object->ctype->ctype;
    }
    if (gGlobal[kLCType]) return gGlobal[kLCType]->ctype;
    return gCRecord.ctype;
}

const TimeTables& CurrentTime() {
    if (void* locale = threadLocale) {
        const LocaleObject* object = (const LocaleObject*)locale;
        if (object->time) return object->time->time;
    }
    if (gGlobal[kLCTime]) return gGlobal[kLCTime]->time;
    return gCRecord.time;
}

// The items of a loaded block's string list (a class's or a map's names): NUL-separated, ended by an
// empty string, as localedef writes them.
unsigned StringListCount(const char* list) {
    unsigned count = 0;
    while (list && *list) {
        list += strlen(list) + 1;
        ++count;
    }
    return count;
}

// Whether a codeset name from the data means UTF-8. glibc compares codeset names through its
// conversion aliases; for this one question the letters, the digits and the case are the whole name,
// so "UTF-8", "utf8" and "UTF_8" are the same.
bool CodesetIsUtf8(const char* codeset) {
    char plain[16];
    size_t length = 0;
    for (const char* p = codeset; *p && length + 1 < sizeof plain; ++p) {
        char c = *p;
        if (c == '-' || c == '_' || c == ' ' || c == '.') continue;
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        plain[length++] = c;
    }
    plain[length] = 0;
    return !strcmp(plain, "utf8");
}

// One item of a block as a string (the block copied it, so it is NUL-terminated).
const char* ItemString(const locale_data::Block& block, unsigned item) {
    return (const char*)block.item[item];
}

void LoadCtype(LocaleRecord* record) {
    CtypeTables& tables = record->ctype;
    if (!locale_data::Open(record->name, kLCType, &record->ctypeBlock)) {
        // No data: a UTF-8 name gets the Unicode tables rather than the C locale's ASCII-only
        // answers, so text still folds and classifies on a system that ships no locales.
        if (tables.utf8) tables.kind = kUnicodeData;
        return;
    }
    locale_data::Block& block = record->ctypeBlock;
    bool complete = true;
    static const unsigned kRequired[] = {kCtypeClassItem,       kCtypeToUpperItem, kCtypeToLowerItem,
                                         kCtypeClassNamesItem,  kCtypeMapNamesItem, kCtypeClassOffsetItem,
                                         kCtypeMapOffsetItem};
    for (unsigned item : kRequired) complete = complete && locale_data::ReadItem(&block, item);
    if (!complete) {  // data this layer cannot use: the C tables answer, as they did before
        locale_data::Close(&block);
        return;
    }
    const unsigned classOffset = *(const uint32_t*)block.item[kCtypeClassOffsetItem];
    const unsigned mapOffset = *(const uint32_t*)block.item[kCtypeMapOffsetItem];
    const char* classNames = ItemString(block, kCtypeClassNamesItem);
    const char* mapNames = ItemString(block, kCtypeMapNamesItem);
    const unsigned classCount = StringListCount(classNames);
    const unsigned mapCount = StringListCount(mapNames);
    if (classCount > (unsigned)kMaxClassTables || mapCount > (unsigned)kMaxMapTables ||
        classOffset + classCount > block.nstrings || mapOffset + mapCount > block.nstrings) {
        locale_data::Close(&block);
        return;
    }
    for (unsigned i = 0; i < classCount; ++i) {
        if (!locale_data::ReadItem(&block, classOffset + i)) {
            locale_data::Close(&block);
            return;
        }
    }
    for (unsigned i = 0; i < mapCount; ++i) {
        if (!locale_data::ReadItem(&block, mapOffset + i)) {
            locale_data::Close(&block);
            return;
        }
    }
    tables.narrowClass = (const unsigned short*)block.item[kCtypeClassItem] + 128;
    tables.narrowLower = (const int*)block.item[kCtypeToLowerItem] + 128;
    tables.narrowUpper = (const int*)block.item[kCtypeToUpperItem] + 128;
    tables.classNameList = classNames;
    tables.mapNameList = mapNames;
    tables.classCount = classCount;
    tables.mapCount = mapCount;
    for (unsigned i = 0; i < classCount; ++i) tables.classTable[i] = block.item[classOffset + i];
    for (unsigned i = 0; i < mapCount; ++i) tables.mapTable[i] = block.item[mapOffset + i];
    for (int slot = 0; slot < kWctypeCount; ++slot) {
        tables.classItem[slot] = -1;
        const char* name = classNames;
        for (unsigned i = 0; i < classCount && *name; ++i, name += strlen(name) + 1) {
            if (!strcmp(name, kWctypeNames[slot])) {
                tables.classItem[slot] = (int)i;
                break;
            }
        }
    }
    tables.lowerItem = -1;
    tables.upperItem = -1;
    {
        const char* name = mapNames;
        for (unsigned i = 0; i < mapCount && *name; ++i, name += strlen(name) + 1) {
            if (!strcmp(name, "tolower")) tables.lowerItem = (int)i;
            else if (!strcmp(name, "toupper")) tables.upperItem = (int)i;
        }
    }
    tables.kind = kLoadedData;
    if (locale_data::ReadItem(&block, kCtypeMbCurMaxItem))
        tables.mbCurMax = *(const uint32_t*)block.item[kCtypeMbCurMaxItem];
    if (locale_data::ReadItem(&block, kCtypeCodesetItem)) {
        tables.codeset = ItemString(block, kCtypeCodesetItem);
        tables.utf8 = CodesetIsUtf8(tables.codeset);
        if (tables.mbCurMax == 1 && tables.utf8) tables.mbCurMax = 6;
    }
    // Every item is a copy, so the descriptor has done its work.
    locale_data::ReleaseFile(&block);
}

void LoadTime(LocaleRecord* record) {
    if (!locale_data::Open(record->name, kLCTime, &record->timeBlock)) return;
    locale_data::Block& block = record->timeBlock;
    unsigned read = 0;
    for (unsigned item = 0; item < (unsigned)kTimeItemCount; ++item) {
        if (locale_data::ReadItem(&block, item)) {
            record->time.items[item] = ItemString(block, item);
            ++read;
        }
    }
    if (read) {
        record->time.loaded = true;
        locale_data::ReleaseFile(&block);
    } else {
        locale_data::Close(&block);
    }
}

LocaleRecord* Load(const char* name) {
    for (LocaleRecord* record = gRecords; record; record = record->next)
        if (!strcmp(record->name, name)) return record;
    void* memory = malloc(sizeof(LocaleRecord));
    if (!memory) return CRecord();
    LocaleRecord* record = ::new (memory) LocaleRecord{};
    record->next = gRecords;
    gRecords = record;
    SetName(record->name, sizeof record->name, name);
    EnsureTables();
    CtypeTables& ctype = record->ctype;
    ctype.kind = kCData;
    ctype.narrowClass = gCtypeTable + 128;
    ctype.narrowLower = gToLowerTable + 128;
    ctype.narrowUpper = gToUpperTable + 128;
    for (int slot = 0; slot < kWctypeCount; ++slot) ctype.classItem[slot] = slot;
    ctype.utf8 = IsUtf8Codeset(name);
    ctype.mbCurMax = ctype.utf8 ? 6 : 1;
    LoadCtype(record);
    LoadTime(record);
    return record;
}

// Which record a category's name asks for: only LC_CTYPE and LC_TIME have one, because they are the
// two categories this layer serves from the data.
void SelectRecord(int category, const char* name) {
    if (category == kLCType || category == kLCTime) gGlobal[category] = Load(name);
}

// The codeset the running locale answers with, from its LC_CTYPE data or, without data, from the
// name that asked for it.
const char* CurrentCodeset() {
    const CtypeTables& tables = CurrentCtype();
    if (tables.codeset) return tables.codeset;
    return tables.utf8 ? "UTF-8" : "ANSI_X3.4-1968";
}

bool IsUtf8() { return CurrentCtype().utf8; }

// ---- the character classes, answered from the current locale's data -----------------------------

constexpr unsigned kLowAscii = 0x80;

// The class bits of one code point. glibc's isw* functions read the narrow table for ASCII and the
// class's own table above it (wctype/wcfuncs_l.c), and so does this; the fallback has its runs.
bool InClass(const CtypeTables& tables, unsigned wc, int slot) {
    if (wc < kLowAscii) return (tables.narrowClass[wc] & kCtypeBits[slot]) != 0;
    const int item = tables.classItem[slot];
    if (item < 0) return false;
    if (tables.kind == kUnicodeData)
        return (locale_data::FallbackClassMask(wc) & kCtypeBits[item]) != 0;
    if (tables.kind == kCData) return false;
    return locale_data::ClassBit(tables.classTable[item], wc) != 0;
}

// The same for a description from wctype(), which names a class of the data by its index. glibc's
// __iswctype_l looks straight into that table, ASCII included (wctype/iswctype_l.c).
bool InClassItem(const CtypeTables& tables, unsigned wc, int item) {
    if (tables.kind == kUnicodeData)
        return item >= 0 && item < kWctypeCount &&
               (locale_data::FallbackClassMask(wc) & kCtypeBits[item]) != 0;
    if (tables.kind == kCData)
        return wc < kLowAscii && item >= 0 && item < kWctypeCount &&
               (tables.narrowClass[wc] & kCtypeBits[item]) != 0;
    if (item < 0 || item >= (int)tables.classCount) return false;
    return locale_data::ClassBit(tables.classTable[item], wc) != 0;
}

// The code point's case mapping. glibc's towlower and towupper read the map tables for every code
// point, ASCII included, which is what makes Turkish's dotted I come out of tr_TR's data.
unsigned Fold(const CtypeTables& tables, unsigned wc, bool upper) {
    if (tables.kind != kLoadedData) {
        if (wc < kLowAscii) return (unsigned)(upper ? UpperOf((int)wc) : LowerOf((int)wc));
        if (tables.kind == kUnicodeData) return locale_data::FallbackFold(wc, upper);
        return wc;  // the C locale folds the ASCII letters and nothing else
    }
    const int item = upper ? tables.upperItem : tables.lowerItem;
    if (item < 0) return wc;
    return locale_data::MapValue(tables.mapTable[item], wc);
}

// A narrow byte's case mapping, from the record's own narrow table.
int NarrowLower(const CtypeTables& tables, int byte) { return tables.narrowLower[byte]; }
int NarrowUpper(const CtypeTables& tables, int byte) { return tables.narrowUpper[byte]; }

void EnsureTables() {
    if (gTablesReady) return;
    for (int i = 0; i < 384; ++i) {
        const int c = i - 128;
        const int byte = c < 0 ? c + 256 : c;
        gCtypeTable[i] = (unsigned short)Classify(byte);
        gToLowerTable[i] = byte < 0x80 ? LowerOf(byte) : byte;
        gToUpperTable[i] = byte < 0x80 ? UpperOf(byte) : byte;
    }
    // The C locale's record: the tables just built, the twelve classes of those tables, and the
    // names kCTimeItems holds. It is the answer before any setlocale call, and for "C" itself.
    gCRecord.ctype.kind = kCData;
    gCRecord.ctype.narrowClass = gCtypeTable + 128;
    gCRecord.ctype.narrowLower = gToLowerTable + 128;
    gCRecord.ctype.narrowUpper = gToUpperTable + 128;
    for (int slot = 0; slot < kWctypeCount; ++slot) gCRecord.ctype.classItem[slot] = slot;
    gCRecord.ctype.utf8 = false;
    gCRecord.ctype.mbCurMax = 1;
    SetName(gCRecord.name, sizeof gCRecord.name, "C");
    gTablesReady = true;
}

// ---- the langinfo strings ----------------------------------------------------------------------

// The C locale's langinfo strings, and a loaded block's LC_TIME items. An item neither defines is
// empty, as glibc's is.
char* LangInfoFor(int item, const TimeTables& time, const char* codeset) {
    switch (item) {
        case kCodeset: return (char*)codeset;
        case kRadixChar: return kPoint;
        case kThouSep: return kEmpty;
        case kCrncYstr: return (char*)"-";
        case kYesExpr: return (char*)"^[yY]";
        case kNoExpr: return (char*)"^[nN]";
        default: break;
    }
    const int index = item - kTimeItem;
    if (item >= kTimeItem && index < kTimeItemCount)
        return (char*)(time.items[index] ? time.items[index] : kCTimeItems[index]);
    return kEmpty;
}

char* LangInfo(int item) { return LangInfoFor(item, CurrentTime(), CurrentCodeset()); }

// ---- the multibyte conversions -----------------------------------------------------------------

// UTF-8 as glibc's C.UTF-8 decodes it: overlong forms and surrogates are refused, while the longer
// forms of the old encoding (5 and 6 bytes) and values above U+10FFFF are taken, which is what
// glibc's own checks accept. The first continuation's range depends on the leading byte, which is
// how overlong forms and surrogates are caught without remembering the length.
size_t Utf8ToWide(wchar_t* out, const char* s, size_t n, MbState& state) {
    unsigned value;
    int need;
    size_t at = 0;
    if (state.count > 0) {  // carrying on with a sequence already started
        value = state.state.value;
        need = state.count;
        state.count = 0;
    } else {
        const unsigned char lead = (unsigned char)s[at];
        if (lead < 0x80) {
            if (out) *out = lead;
            return lead ? 1 : 0;
        }
        unsigned low = 0x80, high = 0xBF;  // the range the first continuation may take
        if (lead < 0xC2) {
            errno = EILSEQ;  // a continuation byte on its own, or an overlong two-byte form
            return (size_t)-1;
        } else if (lead <= 0xDF) {
            value = lead & 0x1F;
            need = 1;
        } else if (lead <= 0xEF) {
            value = lead & 0x0F;
            need = 2;
            if (lead == 0xE0) low = 0xA0;        // overlong
            else if (lead == 0xED) high = 0x9F;  // surrogates
        } else if (lead <= 0xF7) {
            value = lead & 0x07;
            need = 3;
            if (lead == 0xF0) low = 0x90;  // overlong
        } else if (lead <= 0xFB) {
            value = lead & 0x03;
            need = 4;
        } else if (lead <= 0xFD) {
            value = lead & 0x01;
            need = 5;
        } else {
            errno = EILSEQ;
            return (size_t)-1;
        }
        ++at;
        if (at == n) {
            state.count = need;
            state.state.value = value;
            return (size_t)-2;
        }
        const unsigned char first = (unsigned char)s[at];
        if (first < low || first > high) {
            errno = EILSEQ;
            return (size_t)-1;
        }
    }
    while (need) {
        if (at == n) {
            state.count = need;
            state.state.value = value;
            return (size_t)-2;
        }
        const unsigned char byte = (unsigned char)s[at];
        if ((byte & 0xC0) != 0x80) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        value = (value << 6) | (byte & 0x3F);
        ++at;
        --need;
    }
    if (out) *out = (wchar_t)value;
    return at;
}

// One wide character as UTF-8: the shortest form, up to glibc's six bytes for the old encoding.
size_t WideToUtf8(char* out, unsigned value) {
    if (value < 0x80) {
        out[0] = (char)value;
        return 1;
    }
    if ((value >= 0xD800 && value <= 0xDFFF) || value > 0x7FFFFFFF) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    size_t length;
    unsigned lead;
    if (value < 0x800) {
        length = 2;
        lead = 0xC0;
    } else if (value < 0x10000) {
        length = 3;
        lead = 0xE0;
    } else if (value < 0x200000) {
        length = 4;
        lead = 0xF0;
    } else if (value < 0x4000000) {
        length = 5;
        lead = 0xF8;
    } else {
        length = 6;
        lead = 0xFC;
    }
    for (size_t i = length; i-- > 0;) {
        if (i == 0) {
            out[0] = (char)(lead | value);
            break;
        }
        out[i] = (char)(0x80 | (value & 0x3F));
        value >>= 6;
    }
    return length;
}

// ---- strftime ---------------------------------------------------------------------------------

// The output buffer, written the way glibc's strftime writes it: a character that does not fit
// (with room left for the terminating NUL) stops everything, the caller is told 0, and whatever was
// written before it stays. `wide` marks the wcsftime path, whose fields fold case as wide characters
// where the narrow one folds bytes (glibc's two engines differ the same way).
struct Sink {
    char* buffer;
    size_t size;
    size_t used = 0;
    bool failed = false;
    bool wide = false;

    void Put(char c) {
        if (failed) return;
        if (used + 1 >= size) {
            failed = true;
            return;
        }
        buffer[used++] = c;
    }
    void PutText(const char* text, size_t length) {
        for (size_t i = 0; i < length; ++i) Put(text[i]);
    }
    void PutText(const char* text) { PutText(text, strlen(text)); }
    // One conversion's whole output, which glibc writes only when all of it fits; a literal run is
    // written a character at a time, but a value never comes out in part.
    bool Reserve(size_t length) {
        if (failed) return false;
        if (used + length + 1 > size) {
            failed = true;
            return false;
        }
        return true;
    }
};

// The flags a conversion may carry: - (no padding), _ (spaces), 0 (zeros), ^ (uppercase), # (change
// case: uppercase, except that %p comes out lowercase), and a width.
struct Flags {
    bool dash = false;
    bool space = false;
    bool zero = false;
    bool upper = false;
    bool shift = false;
    int width = 0;
};

enum Case { kCaseAsIs, kCaseUpper, kCaseLower };

// One field's text with its case changed as wide characters: glibc's wcsftime folds the wide
// characters it writes, so a Cyrillic or Greek month name changes case there, while its strftime
// folds bytes and leaves the multibyte sequences alone. The text is UTF-8, which is what a UTF-8
// locale's data holds; a byte that is not part of a sequence is folded on its own.
void FoldField(char* out, size_t size, const char* text, bool upper) {
    const size_t length = strlen(text);
    const CtypeTables& tables = CurrentCtype();
    size_t at = 0;
    for (const char* p = text; *p;) {
        MbState state{0, {0}};
        wchar_t wc = 0;
        size_t used = Utf8ToWide(&wc, p, length - (size_t)(p - text), state);
        if (used == (size_t)-1 || used == (size_t)-2) {
            used = 1;
            wc = (wchar_t)(unsigned char)*p;
        }
        char bytes[8];
        const size_t written = WideToUtf8(bytes, Fold(tables, (unsigned)wc, upper));
        if (written == (size_t)-1 || at + written + 1 > size) {
            out[at] = 0;
            return;
        }
        memcpy(out + at, bytes, written);
        at += written;
        p += used;
    }
    out[at] = 0;
}

// One field with glibc's padding and case: `defaultWidth` and `defaultPad` are the conversion's own,
// a width given in the format wins, and '-' turns the padding off (spaces when a width still asks
// for some). The case is glibc's own rule for the narrow engine: each byte through the running
// LC_CTYPE's narrow table, so an 8-bit locale's accented letters change case and a UTF-8 locale's
// bytes above 0x7F (which are the middle of a character) do not. The whole field is written or none
// of it.
void PutField(Sink& out, const char* text, size_t length, const Flags& flags, int defaultWidth,
              char defaultPad, Case textCase) {
    char folded[256];
    if (out.wide && textCase != kCaseAsIs && length < sizeof folded) {
        FoldField(folded, sizeof folded, text, textCase == kCaseUpper);
        text = folded;
        length = strlen(folded);
    }
    const char pad = flags.zero ? '0' : (flags.space || flags.dash) ? ' ' : defaultPad;
    const int field = flags.width ? flags.width : (flags.dash ? 0 : defaultWidth);
    const size_t total = length > (size_t)field ? length : (size_t)field;
    if (!out.Reserve(total)) return;
    for (size_t i = length; i < total; ++i) out.Put(pad);
    const CtypeTables& tables = CurrentCtype();
    for (size_t i = 0; i < length; ++i) {
        char c = text[i];
        if (textCase == kCaseUpper) c = (char)tables.narrowUpper[(unsigned char)c];
        else if (textCase == kCaseLower) c = (char)tables.narrowLower[(unsigned char)c];
        out.Put(c);
    }
}

Case CaseOf(const Flags& flags) {
    if (flags.upper || flags.shift) return kCaseUpper;
    return kCaseAsIs;
}

void PutNumber(Sink& out, long long value, int width, char pad) {
    const bool negative = value < 0;
    const unsigned long long magnitude =
        negative ? 0ULL - (unsigned long long)value : (unsigned long long)value;
    char digits[24];
    int count = 0;
    unsigned long long rest = magnitude;
    do {
        digits[count++] = (char)('0' + rest % 10);
        rest /= 10;
    } while (rest);
    // The padding comes before the sign, as printf's does, and a number is written whole or not at
    // all.
    const size_t total = (size_t)count + (negative ? 1 : 0);
    if (!out.Reserve(total > (size_t)width ? total : (size_t)width)) return;
    for (size_t i = total; i < (size_t)width; ++i) out.Put(pad);
    if (negative) out.Put('-');
    while (count) out.Put(digits[--count]);
}

void PutNumberField(Sink& out, long long value, const Flags& flags, int defaultWidth, char defaultPad) {
    const char pad = flags.zero ? '0' : (flags.space || flags.dash) ? ' ' : defaultPad;
    const int field = flags.width ? flags.width : (flags.dash ? 0 : defaultWidth);
    PutNumber(out, value, field, pad);
}

void PutString(Sink& out, const char* text, const Flags& flags) {
    PutField(out, text, strlen(text), flags, 0, ' ', CaseOf(flags));
}

constexpr bool IsLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

// Days since 1970-01-01 in the proleptic Gregorian calendar, for years far outside tm's.
long long DaysFromCivil(int year, int month, int day) {
    const long long y = (long long)year - (month <= 2 ? 1 : 0);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yearOfEra = y - era * 400;
    const long long dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const long long dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

// The weekday of January 1, 0 = Sunday (1970-01-01 was a Thursday).
int JanuaryFirstWeekday(int year) {
    const long long days = DaysFromCivil(year, 1, 1);
    return (int)(((days % 7) + 7 + 4) % 7);
}

// The weeks an ISO year has: 53 when it starts on a Thursday, or when it is a leap year that starts
// on a Wednesday.
int IsoWeeksInYear(int year) {
    const int first = JanuaryFirstWeekday(year);
    return 52 + (first == 4 || (IsLeapYear(year) && first == 3) ? 1 : 0);
}

void FormatTimeInto(Sink& out, const char* format, const Tm& t, const TimeTables& time);

// The conversions that stand for a whole small format of their own (%c, %x and the rest).
void FormatNested(Sink& out, const char* subFormat, const Tm& t, const Flags& flags,
                  const TimeTables& time) {
    char text[512];
    Sink inner{text, sizeof text};
    FormatTimeInto(inner, subFormat, t, time);
    if (inner.failed) {
        out.failed = true;
        return;
    }
    PutField(out, text, inner.used, flags, 0, ' ', CaseOf(flags));
}

// One LC_TIME item: the loaded locale's string, or the C locale's when its block did not carry it.
const char* TimeString(const TimeTables& time, int item) {
    return time.items[item] ? time.items[item] : kCTimeItems[item];
}

void FormatTimeInto(Sink& out, const char* format, const Tm& t, const TimeTables& time) {
    const int year = t.tm_year + 1900;
    const int month = t.tm_mon >= 0 && t.tm_mon < 12 ? t.tm_mon : 0;
    const int weekday = t.tm_wday >= 0 && t.tm_wday < 7 ? t.tm_wday : 0;
    const int hour12 = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
    // glibc's %C takes the century towards minus infinity (-999 is century -10) and %y is what is
    // left, so both agree for years before 1000; %Y and %G print the year with no padding at all.
    const int century = year >= 0 ? year / 100 : (year - 99) / 100;
    const int yearOfCentury = year - century * 100;
    // The ISO week and year: a week belongs to the year its Thursday falls in. The weekday counts
    // from Monday (0 to 6) and the week is the one of the first Thursday's year.
    const int isoWeekday = (weekday + 6) % 7;
    int isoWeek = (t.tm_yday - isoWeekday + 10) / 7;
    int isoYear = year;
    if (isoWeek < 1) {
        isoYear = year - 1;
        isoWeek = IsoWeeksInYear(isoYear);
    } else if (isoWeek > IsoWeeksInYear(year)) {
        isoYear = year + 1;
        isoWeek = 1;
    }

    const char* p = format;
    while (*p && !out.failed) {
        if (*p != '%') {
            out.Put(*p++);
            continue;
        }
        const char* specStart = p;
        ++p;
        Flags flags;
        for (;; ++p) {
            if (*p == '-') flags.dash = true;
            else if (*p == '_') flags.space = true;
            else if (*p == '0') flags.zero = true;
            else if (*p == '^') flags.upper = true;
            else if (*p == '#') flags.shift = true;
            else break;
        }
        while (*p >= '0' && *p <= '9') {
            if (flags.width < 100000) flags.width = flags.width * 10 + (*p - '0');
            ++p;
        }
        if (*p == 'E' || *p == 'O') ++p;  // no era years or alternative digits: the modifier is ignored
        if (!*p) {                        // a lone spec at the end: copied out as it stands
            for (const char* q = specStart; q < p; ++q) out.Put(*q);
            break;
        }
        const char conversion = *p++;
        switch (conversion) {
            case '%': out.Put('%'); break;
            case 'a': PutString(out, TimeString(time, kAbDayItem + weekday), flags); break;
            case 'A': PutString(out, TimeString(time, kDayItem + weekday), flags); break;
            case 'b':
            case 'h': PutString(out, TimeString(time, kAbMonItem + month), flags); break;
            case 'B': PutString(out, TimeString(time, kMonItem + month), flags); break;
            case 'c': FormatNested(out, TimeString(time, kDateTimeFormat), t, flags, time); break;
            case 'C': PutNumberField(out, century, flags, 0, '0'); break;
            case 'd': PutNumberField(out, t.tm_mday, flags, 2, '0'); break;
            case 'D': FormatNested(out, "%m/%d/%y", t, flags, time); break;
            case 'e': PutNumberField(out, t.tm_mday, flags, 2, ' '); break;
            case 'F': FormatNested(out, "%Y-%m-%d", t, flags, time); break;
            case 'g': PutNumberField(out, isoYear - (isoYear >= 0 ? isoYear / 100 : (isoYear - 99) / 100) * 100,
                                     flags, 2, '0');
                break;
            case 'G': PutNumberField(out, isoYear, flags, 0, '0'); break;
            case 'H': PutNumberField(out, t.tm_hour, flags, 2, '0'); break;
            case 'I': PutNumberField(out, hour12, flags, 2, '0'); break;
            case 'j': PutNumberField(out, t.tm_yday + 1, flags, 3, '0'); break;
            case 'k': PutNumberField(out, t.tm_hour, flags, 2, ' '); break;
            case 'l': PutNumberField(out, hour12, flags, 2, ' '); break;
            case 'm': PutNumberField(out, t.tm_mon + 1, flags, 2, '0'); break;
            case 'M': PutNumberField(out, t.tm_min, flags, 2, '0'); break;
            case 'n': PutString(out, "\n", flags); break;
            case 'p': {  // '#' is glibc's lowercase am/pm
                const char* ampm = TimeString(time, t.tm_hour < 12 ? kAmItem : kPmItem);
                PutField(out, ampm, strlen(ampm), flags, 0, ' ',
                         flags.shift ? kCaseLower : flags.upper ? kCaseUpper : kCaseAsIs);
                break;
            }
            case 'P': {
                const char* ampm = TimeString(time, t.tm_hour < 12 ? kAmItem : kPmItem);
                PutField(out, ampm, strlen(ampm), flags, 0, ' ', kCaseLower);
                break;
            }
            case 'r': {
                // glibc uses the locale's own 12-hour format, and its C answer when the locale has
                // none (time/strftime_l.c's %r).
                const char* subFormat = TimeString(time, kTimeFormatAmPm);
                FormatNested(out, *subFormat ? subFormat : "%I:%M:%S %p", t, flags, time);
                break;
            }
            case 'R': FormatNested(out, "%H:%M", t, flags, time); break;
            case 's': {
                // glibc's answer: the epoch seconds of the tm, read through mktime so that fields
                // out of range are normalised and a daylight flag is honoured.
                errno = 0;
                Tm copy = t;
                const long long seconds = (long long)mktime(&copy);
                if (seconds == -1 && errno == EOVERFLOW) {
                    out.failed = true;
                    break;
                }
                PutNumberField(out, seconds, flags, 1, ' ');
                break;
            }
            case 'S': PutNumberField(out, t.tm_sec, flags, 2, '0'); break;
            case 't': PutString(out, "\t", flags); break;
            case 'T': FormatNested(out, "%H:%M:%S", t, flags, time); break;
            case 'u': PutNumberField(out, weekday == 0 ? 7 : weekday, flags, 1, '0'); break;
            case 'U': PutNumberField(out, (t.tm_yday + 7 - weekday) / 7, flags, 2, '0'); break;
            case 'V': PutNumberField(out, isoWeek, flags, 2, '0'); break;
            case 'w': PutNumberField(out, weekday, flags, 1, '0'); break;
            case 'W': PutNumberField(out, (t.tm_yday + 7 - (weekday + 6) % 7) / 7, flags, 2, '0'); break;
            case 'x': FormatNested(out, TimeString(time, kDateFormat), t, flags, time); break;
            case 'X': FormatNested(out, TimeString(time, kTimeFormat), t, flags, time); break;
            case 'y': PutNumberField(out, yearOfCentury, flags, 2, '0'); break;
            case 'Y': PutNumberField(out, year, flags, 0, '0'); break;
            case 'Z':
                PutString(out, t.tm_zone ? t.tm_zone : cordinal_zone_name(t.tm_isdst > 0), flags);
                break;
            case 'z': {
                if (t.tm_isdst < 0) break;  // glibc writes nothing for an unknown daylight flag
                char text[16];
                int length = 0;
                const long offset = t.tm_gmtoff;
                const unsigned long magnitude = (unsigned long)(offset < 0 ? -offset : offset);
                text[length++] = offset < 0 ? '-' : '+';
                char digits[12];
                int count = 0;
                unsigned long hours = magnitude / 3600;
                const unsigned long minutes = (magnitude / 60) % 60;
                do {
                    digits[count++] = (char)('0' + hours % 10);
                    hours /= 10;
                } while (hours);
                while (count < 2) digits[count++] = '0';
                while (count) text[length++] = digits[--count];
                text[length++] = (char)('0' + minutes / 10);
                text[length++] = (char)('0' + minutes % 10);
                PutField(out, text, (size_t)length, flags, 5, ' ', kCaseAsIs);
                break;
            }
            default:
                // A conversion it does not know is copied out exactly as it stands.
                for (const char* q = specStart; q < p; ++q) out.Put(*q);
                break;
        }
    }
}

// wcsftime formats through the narrow engine: the format is written out as UTF-8 and the result read
// back the same way, which is exact for the ASCII formats time formatting uses and for literals in
// any script.
bool NarrowFormat(char* out, size_t size, const wchar_t* format) {
    size_t at = 0;
    for (const wchar_t* p = format; *p; ++p) {
        char bytes[8];
        size_t length;
        if (*p < 0x80) {
            bytes[0] = (char)*p;
            length = 1;
        } else {
            length = WideToUtf8(bytes, (unsigned)*p);
            if (length == (size_t)-1) return false;
        }
        if (at + length + 1 >= size) return false;
        memcpy(out + at, bytes, length);
        at += length;
    }
    out[at] = 0;
    return true;
}

}  // namespace

namespace locale_data {

// strcasecmp and strncasecmp (string.cpp) fold through this, which is glibc's own rule: its
// __strcasecmp_l calls __tolower_l for each byte, and that reads the running LC_CTYPE's narrow map.
// The table is asked for once per string, so the fold costs a load rather than a call per byte.
const int* NarrowToLowerTable() { return CurrentCtype().narrowLower; }

}  // namespace locale_data

#define HIDE(name) __asm__(".hidden " #name);
HIDE(setlocale) HIDE(localeconv) HIDE(nl_langinfo) HIDE(nl_langinfo_l) HIDE(__nl_langinfo_l)
HIDE(newlocale) HIDE(__newlocale) HIDE(duplocale) HIDE(__duplocale) HIDE(freelocale) HIDE(__freelocale)
HIDE(uselocale) HIDE(__uselocale) HIDE(__ctype_get_mb_cur_max)
HIDE(isalnum) HIDE(isalpha) HIDE(isspace) HIDE(isxdigit) HIDE(tolower) HIDE(toupper)
HIDE(iswalnum) HIDE(iswalpha) HIDE(iswdigit) HIDE(iswspace) HIDE(iswxdigit)
HIDE(towlower) HIDE(towupper) HIDE(towlower_l) HIDE(towupper_l) HIDE(__towlower_l) HIDE(__towupper_l)
HIDE(__wctype_l) HIDE(__iswctype_l) HIDE(wcscasecmp) HIDE(wcsncasecmp)
HIDE(mbrtowc) HIDE(wcrtomb) HIDE(btowc) HIDE(wctob) HIDE(mbsrtowcs) HIDE(mbsnrtowcs) HIDE(wcsnrtombs)
HIDE(__mbsrtowcs_chk)
HIDE(__strcoll_l) HIDE(__strxfrm_l) HIDE(__wcscoll_l) HIDE(__wcsxfrm_l)
HIDE(strftime) HIDE(__strftime_l) HIDE(wcsftime) HIDE(__wcsftime_l)
#undef HIDE

extern "C" {
char* setlocale(int category, const char* name) {
    if (category < 0 || category > kLCIdentification) return nullptr;
    if (!name) return NameOf(category);
    if (!*name) name = ResolveName(category);  // "" asks the environment
    if (!strcmp(name, "POSIX")) name = "C";    // glibc's other name for the C locale
    StoreName(category, name);
    return NameOf(category);
}

Lconv* localeconv(void) { return &gLconv; }

char* nl_langinfo(int item) { return LangInfo(item); }
char* nl_langinfo_l(int item, void* locale) {
    const LocaleObject* object = (const LocaleObject*)locale;
    if (!object || locale == (void*)-1) return LangInfo(item);
    const TimeTables& time = object->time ? object->time->time : gCRecord.time;
    return LangInfoFor(item, time, object->codeset);
}
char* __nl_langinfo_l(int item, void* locale) { return nl_langinfo_l(item, locale); }

// A locale object carries its LC_CTYPE's tables (which libstdc++'s ctype facets read straight out of
// it), its LC_TIME data, and the codeset it answers with. A mask bit for another category changes
// nothing: those categories are the C locale's in every object.
void* newlocale(int mask, const char* name, void* base) {
    // The categories' bits, LC_ALL's own bit (which libstdc++ uses as a mask) and nothing else.
    if (mask < 0 || ((uint32_t)mask & ~(0x1FBFu | (1u << kLCAll))) != 0) {
        errno = EINVAL;
        return nullptr;
    }
    if (name && !*name) name = ResolveName(kLCType);
    if (name && !strcmp(name, "POSIX")) name = "C";
    void* const global = (void*)-1;
    const bool haveBase = base && base != global;
    LocaleObject* object = (LocaleObject*)malloc(sizeof(LocaleObject));
    if (!object) return nullptr;
    if (haveBase) {
        memcpy(object, base, sizeof(LocaleObject));
    } else {
        for (int i = 0; i < kCategoryCount; ++i) {
            object->locales[i] = nullptr;
            object->names[i] = kEmpty;
        }
        object->ctype = CRecord();
        object->time = CRecord();
        object->codeset[0] = 0;
    }
    if (object->time == nullptr) object->time = CRecord();
    if (object->ctype == nullptr) object->ctype = CRecord();
    if (name) {
        LocaleRecord* loaded = Load(name);
        const uint32_t bits = (uint32_t)mask == (1u << kLCAll) ? 0x1FBFu : (uint32_t)mask;
        if (bits & (1u << kLCType)) object->ctype = loaded;
        if (bits & (1u << kLCTime)) object->time = loaded;
    }
    const CtypeTables& tables = object->ctype->ctype;
    object->ctypeTable = tables.narrowClass;
    object->tolowerTable = tables.narrowLower;
    object->toupperTable = tables.narrowUpper;
    if (tables.codeset) {
        SetName(object->codeset, sizeof object->codeset, tables.codeset);
    } else if (haveBase && !name) {
        SetName(object->codeset, sizeof object->codeset, ((LocaleObject*)base)->codeset);
    } else {
        SetName(object->codeset, sizeof object->codeset,
                name ? (tables.utf8 ? "UTF-8" : "ANSI_X3.4-1968") : CurrentCodeset());
    }
    return object;
}
void* __newlocale(int mask, const char* name, void* base) { return newlocale(mask, name, base); }

void* duplocale(void* locale) {
    EnsureTables();
    LocaleObject* object = (LocaleObject*)malloc(sizeof(LocaleObject));
    if (!object) return nullptr;
    if (!locale || locale == (void*)-1) {  // LC_GLOBAL_LOCALE, or nothing: the current global one
        for (int i = 0; i < kCategoryCount; ++i) {
            object->locales[i] = nullptr;
            object->names[i] = kEmpty;
        }
        object->ctype = gGlobal[kLCType] ? gGlobal[kLCType] : CRecord();
        object->time = gGlobal[kLCTime] ? gGlobal[kLCTime] : CRecord();
        SetName(object->codeset, sizeof object->codeset, CurrentCodeset());
    } else {
        memcpy(object, locale, sizeof(LocaleObject));
        return object;
    }
    const CtypeTables& tables = object->ctype->ctype;
    object->ctypeTable = tables.narrowClass;
    object->tolowerTable = tables.narrowLower;
    object->toupperTable = tables.narrowUpper;
    return object;
}
void* __duplocale(void* locale) { return duplocale(locale); }

void freelocale(void* locale) {
    if (!locale || locale == (void*)-1) return;  // LC_GLOBAL_LOCALE has nothing to free
    free(locale);
}
void __freelocale(void* locale) { freelocale(locale); }

// The thread's own locale, which uselocale(0) reports: the ctype and time functions read its records
// instead of the global locale's. LC_GLOBAL_LOCALE puts the thread back on the global locale.
void* uselocale(void* locale) {
    void* previous = threadLocale ? threadLocale : (void*)-1;
    if (locale) threadLocale = locale == (void*)-1 ? nullptr : locale;
    return previous;
}
void* __uselocale(void* locale) { return uselocale(locale); }

size_t __ctype_get_mb_cur_max(void) { return CurrentCtype().mbCurMax; }

int isalnum(int c) {
    int byte;
    return ByteOf(c, byte) && (CurrentCtype().narrowClass[byte] & kAlnum) ? 1 : 0;
}
int isalpha(int c) {
    int byte;
    return ByteOf(c, byte) && (CurrentCtype().narrowClass[byte] & kAlpha) ? 1 : 0;
}
int isspace(int c) {
    int byte;
    return ByteOf(c, byte) && (CurrentCtype().narrowClass[byte] & kSpace) ? 1 : 0;
}
int isxdigit(int c) {
    int byte;
    return ByteOf(c, byte) && (CurrentCtype().narrowClass[byte] & kXdigit) ? 1 : 0;
}
int tolower(int c) {
    int byte;
    if (c == -1 || !ByteOf(c, byte)) return c;
    return NarrowLower(CurrentCtype(), byte);
}
int toupper(int c) {
    int byte;
    if (c == -1 || !ByteOf(c, byte)) return c;
    return NarrowUpper(CurrentCtype(), byte);
}

int iswalnum(wint_t c) { return InClass(CurrentCtype(), c, kWctypeAlnum) ? 1 : 0; }
int iswalpha(wint_t c) { return InClass(CurrentCtype(), c, kWctypeAlpha) ? 1 : 0; }
int iswdigit(wint_t c) { return InClass(CurrentCtype(), c, kWctypeDigit) ? 1 : 0; }
int iswspace(wint_t c) { return InClass(CurrentCtype(), c, kWctypeSpace) ? 1 : 0; }
int iswxdigit(wint_t c) { return InClass(CurrentCtype(), c, kWctypeXdigit) ? 1 : 0; }
wint_t towlower(wint_t c) { return (wint_t)Fold(CurrentCtype(), c, false); }
wint_t towupper(wint_t c) { return (wint_t)Fold(CurrentCtype(), c, true); }

// The _l forms read the object's own LC_CTYPE (or the current locale's when there is no object),
// which is how libstdc++ asks: a uselocale'd object carries its locale's tables.
const CtypeTables& CtypeOf(void* locale) {
    const LocaleObject* object = (const LocaleObject*)locale;
    if (object && locale != (void*)-1 && object->ctype) return object->ctype->ctype;
    return CurrentCtype();
}

wint_t towlower_l(wint_t c, void* locale) { return (wint_t)Fold(CtypeOf(locale), c, false); }
wint_t towupper_l(wint_t c, void* locale) { return (wint_t)Fold(CtypeOf(locale), c, true); }
wint_t __towlower_l(wint_t c, void* locale) { return towlower_l(c, locale); }
wint_t __towupper_l(wint_t c, void* locale) { return towupper_l(c, locale); }

// wctype's answer: the class's index in the locale's own name list, plus one, as glibc's
// __wctype_l reads the list (wctype/wctype_l.c). Zero means the locale has no such class. The
// fallback has the twelve C names; a loaded locale also has glibc's combining classes.
unsigned long __wctype_l(const char* name, void* locale) {
    const CtypeTables& tables = CtypeOf(locale);
    if (tables.kind == kUnicodeData) {
        for (int slot = 0; slot < kWctypeCount; ++slot)
            if (!strcmp(name, kWctypeNames[slot])) return (unsigned long)(slot + 1);
        return 0;
    }
    if (tables.kind == kCData) {
        for (int slot = 0; slot < kWctypeCount; ++slot)
            if (!strcmp(name, kWctypeNames[slot])) return (unsigned long)(slot + 1);
        return 0;
    }
    const char* entry = tables.classNameList;
    for (unsigned i = 0; i < tables.classCount && entry && *entry; ++i, entry += strlen(entry) + 1)
        if (!strcmp(name, entry)) return (unsigned long)(i + 1);
    return 0;
}

int __iswctype_l(wint_t c, unsigned long desc, void* locale) {
    if (!desc) return 0;
    return InClassItem(CtypeOf(locale), c, (int)desc - 1) ? 1 : 0;
}

int wcscasecmp(const wchar_t* a, const wchar_t* b) {
    const CtypeTables& tables = CurrentCtype();
    for (;; ++a, ++b) {
        const int left = (int)Fold(tables, *a, false);
        const int right = (int)Fold(tables, *b, false);
        if (left != right || !left) return left - right;
    }
}
int wcsncasecmp(const wchar_t* a, const wchar_t* b, size_t n) {
    const CtypeTables& tables = CurrentCtype();
    for (size_t i = 0; i < n; ++i) {
        const int left = (int)Fold(tables, a[i], false);
        const int right = (int)Fold(tables, b[i], false);
        if (left != right || !left) return left - right;
    }
    return 0;
}

// The multibyte conversions: UTF-8 in a UTF-8 locale, single bytes in the C locale.
size_t mbrtowc(wchar_t* out, const char* s, size_t n, MbState* state) {
    static thread_local MbState internal;
    if (!state) state = &internal;
    if (!s) {  // glibc's reset form: mbrtowc(NULL, "", 1, ps)
        state->count = 0;
        state->state.value = 0;
        if (out) *out = 0;
        return 0;
    }
    if (!n) return (size_t)-2;
    if (!IsUtf8()) {
        const unsigned char byte = (unsigned char)s[0];
        if (byte >= 0x80) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (out) *out = byte;
        return byte ? 1 : 0;
    }
    return Utf8ToWide(out, s, n, *state);
}

size_t wcrtomb(char* s, wchar_t wc, MbState* state) {
    static thread_local MbState internal;
    if (!state) state = &internal;
    state->count = 0;
    state->state.value = 0;
    if (!s) {  // glibc's reset form converts the null character into an internal buffer
        if ((unsigned)wc >= 0x80 && !IsUtf8()) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        return 1;
    }
    if (!IsUtf8()) {
        if ((unsigned)wc >= 0x80) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        s[0] = (char)wc;
        return 1;
    }
    const size_t length = WideToUtf8(s, (unsigned)wc);
    if (length == (size_t)-1) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    return length;
}

wint_t btowc(int c) {
    if (c < 0 || c > 0xFF) return (wint_t)-1;
    if (c >= 0x80) return (wint_t)-1;  // single-byte characters only
    return (wint_t)c;
}
int wctob(wint_t c) { return c < 0x80 ? (int)c : -1; }

size_t mbsnrtowcs(wchar_t* dst, const char** src, size_t bytes, size_t length, MbState* state) {
    static thread_local MbState internal;
    if (!state) state = &internal;
    const char* at = *src;
    size_t written = 0;
    while (bytes) {
        if (dst && written == length) {  // the caller's room is used up
            *src = at;
            return written;
        }
        if (*at == 0) {
            if (dst && written < length) dst[written] = 0;
            *src = nullptr;
            return written;
        }
        wchar_t wc = 0;
        size_t used;
        if (IsUtf8()) {
            used = Utf8ToWide(&wc, at, bytes < 8 ? bytes : 8, *state);
        } else {
            const unsigned char byte = (unsigned char)*at;
            if (byte >= 0x80) {
                errno = EILSEQ;
                return (size_t)-1;
            }
            wc = byte;
            used = 1;
        }
        if (used == (size_t)-1 || used == (size_t)-2) {
            errno = EILSEQ;  // an invalid or incomplete sequence in the input
            return (size_t)-1;
        }
        if (dst) dst[written] = wc;
        at += used;
        bytes -= used;
        ++written;
    }
    if (dst && written < length) dst[written] = 0;
    *src = at;
    return written;
}

size_t mbsrtowcs(wchar_t* dst, const char** src, size_t length, MbState* state) {
    return mbsnrtowcs(dst, src, (size_t)-1, length, state);
}

// The fortified form: a caller whose buffer is shorter than the count it asks for is a bug.
size_t __mbsrtowcs_chk(wchar_t* dst, const char** src, size_t length, MbState* state, size_t dstLength) {
    if (dst && length > dstLength) abort();
    return mbsrtowcs(dst, src, length, state);
}

size_t wcsnrtombs(char* dst, const wchar_t** src, size_t count, size_t length, MbState* state) {
    (void)state;
    const wchar_t* at = *src;
    size_t written = 0;
    while (count) {
        if (dst && written == length) {
            *src = at;
            return written;
        }
        if (*at == 0) {
            if (dst && written < length) dst[written] = 0;
            *src = nullptr;
            return written;
        }
        char bytes[8];
        size_t used;
        if (IsUtf8()) {
            used = WideToUtf8(bytes, (unsigned)*at);
            if (used == (size_t)-1) {
                errno = EILSEQ;
                return (size_t)-1;
            }
        } else {
            if ((unsigned)*at >= 0x80) {
                errno = EILSEQ;
                return (size_t)-1;
            }
            bytes[0] = (char)*at;
            used = 1;
        }
        if (dst) {  // room for the whole character or none
            if (written + used > length) {
                *src = at;
                return written;
            }
            memcpy(dst + written, bytes, used);
        }
        written += used;
        ++at;
        --count;
    }
    *src = at;
    return written;
}

// The C locale's collation: byte or wide-value order, which is what glibc's C locale does.
int __strcoll_l(const char* a, const char* b, void*) { return strcmp(a, b); }
int __wcscoll_l(const wchar_t* a, const wchar_t* b, void*) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a < *b ? -1 : *a > *b ? 1 : 0;
}
size_t __strxfrm_l(char* dst, const char* src, size_t size, void*) {
    const size_t length = strlen(src);
    if (size) {
        const size_t copied = length < size - 1 ? length : size - 1;
        memcpy(dst, src, copied);
        dst[copied] = 0;
    }
    return length;
}
size_t __wcsxfrm_l(wchar_t* dst, const wchar_t* src, size_t size, void*) {
    size_t length = 0;
    while (src[length]) ++length;
    if (size) {
        const size_t copied = length < size - 1 ? length : size - 1;
        for (size_t i = 0; i < copied; ++i) dst[i] = src[i];
        dst[copied] = 0;
    }
    return length;
}

// The time functions read the thread's locale (else the global one), as glibc's do.
const TimeTables& TimeOf(void* locale) {
    const LocaleObject* object = (const LocaleObject*)locale;
    if (object && locale != (void*)-1 && object->time) return object->time->time;
    return CurrentTime();
}

size_t strftime(char* buffer, size_t size, const char* format, const Tm* tm) {
    Sink sink{buffer, size};
    FormatTimeInto(sink, format, *tm, CurrentTime());
    if (sink.failed) return 0;
    buffer[sink.used] = 0;
    return sink.used;
}
size_t __strftime_l(char* buffer, size_t size, const char* format, const Tm* tm, void* locale) {
    Sink sink{buffer, size};
    FormatTimeInto(sink, format, *tm, TimeOf(locale));
    if (sink.failed) return 0;
    buffer[sink.used] = 0;
    return sink.used;
}
size_t wcsftime(wchar_t* buffer, size_t size, const wchar_t* format, const Tm* tm) {
    char narrowFormat[512];
    if (!NarrowFormat(narrowFormat, sizeof narrowFormat, format)) return 0;
    char text[4096];
    Sink sink{text, sizeof text};
    sink.wide = true;
    FormatTimeInto(sink, narrowFormat, *tm, CurrentTime());
    if (sink.failed) return 0;
    size_t written = 0;
    MbState state{0, {0}};
    for (size_t at = 0; at < sink.used;) {
        wchar_t wc = 0;
        const size_t used = Utf8ToWide(&wc, text + at, sink.used - at, state);
        if (used == (size_t)-1 || used == (size_t)-2 || written + 1 >= size) return 0;
        buffer[written++] = wc;
        at += used ? used : 1;
    }
    buffer[written] = 0;
    return written;
}
size_t __wcsftime_l(wchar_t* buffer, size_t size, const wchar_t* format, const Tm* tm, void* locale) {
    (void)locale;
    return wcsftime(buffer, size, format, tm);
}
}  // extern "C"
