// cordinal's locale layer (locale.cpp) against glibc's, for checks.h: struct lconv's layout and
// every field of localeconv, the C locale's langinfo strings, the narrow and wide character classes
// and case maps over every byte and over the first three thousand code points, the multibyte
// conversions in the C and UTF-8 locales (including split and invalid sequences), the C locale's
// collation, setlocale's names and its compound, the locale objects libstdc++ builds, and strftime
// and wcsftime byte for byte across a spread of tms and formats, truncation included.
#pragma once
#include <dlfcn.h>
#include <errno.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

// The C locale's collation, which locale.cpp defines under glibc's internal names.
extern "C" int __strcoll_l(const char*, const char*, locale_t);
extern "C" size_t __strxfrm_l(char*, const char*, size_t, locale_t);
extern "C" int __wcscoll_l(const wchar_t*, const wchar_t*, locale_t);
extern "C" size_t __wcsxfrm_l(wchar_t*, const wchar_t*, size_t, locale_t);

namespace libc_checks {

// glibc's own copies of the locale functions, reached past cordinal's hidden ones. The data checks
// with the data ones work use these the same way.
namespace locale_glibc {
typedef char* (*SetlocaleFn)(int, const char*);
typedef struct lconv* (*LocaleconvFn)(void);
typedef char* (*NlLanginfoFn)(int);
typedef size_t (*StrftimeFn)(char*, size_t, const char*, const struct tm*);
typedef size_t (*WcsftimeFn)(wchar_t*, size_t, const wchar_t*, const struct tm*);
typedef int (*IsalphaFn)(int);
typedef int (*IsalnumFn)(int);
typedef int (*IsspaceFn)(int);
typedef int (*IsxdigitFn)(int);
typedef int (*TolowerFn)(int);
typedef int (*ToupperFn)(int);
typedef int (*IswalnumFn)(wint_t);
typedef int (*IswalphaFn)(wint_t);
typedef int (*IswspaceFn)(wint_t);
typedef int (*IswxdigitFn)(wint_t);
typedef wint_t (*TowlowerFn)(wint_t);
typedef wint_t (*TowupperFn)(wint_t);
typedef size_t (*MbrtowcFn)(wchar_t*, const char*, size_t, mbstate_t*);
typedef size_t (*WcrtombFn)(char*, wchar_t, mbstate_t*);
typedef wint_t (*BtowcFn)(int);
typedef int (*WctobFn)(wint_t);
typedef size_t (*MbsrtowcsFn)(wchar_t*, const char**, size_t, mbstate_t*);
typedef size_t (*WcsrtombsFn)(char*, const wchar_t**, size_t, mbstate_t*);
typedef size_t (*CtypeMbCurMaxFn)(void);
typedef size_t (*StrxfrmFn)(char*, const char*, size_t);
typedef int (*StrcollFn)(const char*, const char*);
}  // namespace locale_glibc

// The formats strftime and wcsftime are compared over: every conversion the layer has, the flags and
// widths, and the E and O modifiers.
constexpr const char* kStrftimeFormats[] = {
    "%a %A %b %B %C %c %D %d %e %F %G %g %H %h %I %j %k %l %M %m %n %p %P %r %R %S %s %t %T %U %u %V %v %W %w %X %x %Y %y %Z %z %+ %%",
    "%Ec %EC %Ex %EX %Ey %EY %Od %Oe %OH %OI %Om %OM %OS %Ou %OU %OV %Ow %OW %Oy",
    "%-d %_m %0e %^a %#b %10Y %-5Y %^B %#p %#P %^P %5e %-e %3d %_3d %03d %10s %-s %10Z %-Z",
    "% ", "%", "abc%", "%-", "%0", "%Z %z", "%:z", "%v", "%+", "%#", "%-10Y", "%010Y", "%e %k %l",
    "  plain text  ", "%Y-%m-%d %H.%M.%S.txt", "%j %U %W %V %G",
};

// ---- the system's own locale data -----------------------------------------------------------

// The locales a container run builds with localedef (linux/smoke-test.sh): the UTF-8 ones the layer
// serves most, then one 8-bit locale, whose narrow table is the only way the byte folds (strcasecmp,
// the narrow class table) are worth comparing at all, and C.UTF-8 last. A name glibc cannot load is
// skipped, because glibc is the answer being compared to.
constexpr const char* kLocaleBattery[] = {"de_DE.UTF-8", "ru_RU.UTF-8", "el_GR.UTF-8",
                                          "tr_TR.UTF-8", "cs_CZ.UTF-8", "ja_JP.UTF-8",
                                          "en_GB.UTF-8", "de_DE.ISO-8859-1", "C.UTF-8"};
constexpr int kLocaleBatteryCount = (int)(sizeof kLocaleBattery / sizeof kLocaleBattery[0]);

// The LC_TIME items, in <langinfo.h>'s order, which the layer numbers the same way.
constexpr int kTimeLanginfo[] = {
    ABDAY_1,   ABDAY_2,   ABDAY_3,   ABDAY_4,   ABDAY_5,       ABDAY_6,       ABDAY_7,
    DAY_1,     DAY_2,     DAY_3,     DAY_4,     DAY_5,         DAY_6,         DAY_7,
    ABMON_1,   ABMON_2,   ABMON_3,   ABMON_4,   ABMON_5,       ABMON_6,       ABMON_7,
    ABMON_8,   ABMON_9,   ABMON_10,  ABMON_11,  ABMON_12,
    MON_1,     MON_2,     MON_3,     MON_4,     MON_5,         MON_6,         MON_7,
    MON_8,     MON_9,     MON_10,    MON_11,    MON_12,
    AM_STR,    PM_STR,    D_T_FMT,   D_FMT,     T_FMT,         T_FMT_AMPM,
    ERA,       __ERA_YEAR, ERA_D_FMT, ALT_DIGITS, ERA_D_T_FMT, ERA_T_FMT,
};

using IsWideFn = int (*)(wint_t);
using ToWideFn = wint_t (*)(wint_t);
using IsNarrowFn = int (*)(int);
using ToNarrowFn = int (*)(int);
using StrcasecmpFn = int (*)(const char*, const char*);
using StrncasecmpFn = int (*)(const char*, const char*, size_t);
using WcscasecmpFn = int (*)(const wchar_t*, const wchar_t*);
using NewlocaleFn = locale_t (*)(int, const char*, locale_t);
using FreelocaleFn = void (*)(locale_t);

// The same format with its E and O modifiers dropped: what this layer answers for them, since it has
// no era years and no alternative digits (docs/linux-libc.md). glibc answers the stripped format
// exactly when its data has neither.
inline void StripModifiers(const char* format, char* out, size_t size) {
    size_t at = 0;
    for (const char* p = format; *p && at + 1 < size;) {
        out[at++] = *p;
        if (*p++ != '%') continue;
        while (*p && (*p == '-' || *p == '_' || *p == '0' || *p == '^' || *p == '#' || (*p >= '0' && *p <= '9')))
            out[at++] = *p++;
        if (*p == 'E' || *p == 'O') ++p;
    }
    out[at] = 0;
}

// Whether the locale's data carries what this layer does not implement: era entries (%E) or
// alternative digits (%O).
inline bool HasAlternatives(locale_glibc::NlLanginfoFn langinfo) {
    return *langinfo(ERA) || *langinfo(ALT_DIGITS);
}

// The system's own locale data against glibc's: every LC_TIME item, strftime and wcsftime under each
// battery locale, the narrow and wide class and case tables over every byte and the whole plane, the
// Turkish dotted I, wcscasecmp, and the embedded Unicode fallback.
inline void CheckLocaleData(locale_glibc::SetlocaleFn glibcSetlocale,
                            locale_glibc::NlLanginfoFn glibcLanginfo,
                            locale_glibc::StrftimeFn glibcStrftime,
                            locale_glibc::WcsftimeFn glibcWcsftime) {
    using namespace locale_glibc;
    IsWideFn glibcIswalnum = (IsWideFn)dlsym(RTLD_DEFAULT, "iswalnum");
    IsWideFn glibcIswalpha = (IsWideFn)dlsym(RTLD_DEFAULT, "iswalpha");
    IsWideFn glibcIswblank = (IsWideFn)dlsym(RTLD_DEFAULT, "iswblank");
    IsWideFn glibcIswcntrl = (IsWideFn)dlsym(RTLD_DEFAULT, "iswcntrl");
    IsWideFn glibcIswdigit = (IsWideFn)dlsym(RTLD_DEFAULT, "iswdigit");
    IsWideFn glibcIswgraph = (IsWideFn)dlsym(RTLD_DEFAULT, "iswgraph");
    IsWideFn glibcIswlower = (IsWideFn)dlsym(RTLD_DEFAULT, "iswlower");
    IsWideFn glibcIswprint = (IsWideFn)dlsym(RTLD_DEFAULT, "iswprint");
    IsWideFn glibcIswpunct = (IsWideFn)dlsym(RTLD_DEFAULT, "iswpunct");
    IsWideFn glibcIswspace = (IsWideFn)dlsym(RTLD_DEFAULT, "iswspace");
    IsWideFn glibcIswupper = (IsWideFn)dlsym(RTLD_DEFAULT, "iswupper");
    IsWideFn glibcIswxdigit = (IsWideFn)dlsym(RTLD_DEFAULT, "iswxdigit");
    ToWideFn glibcTowlower = (ToWideFn)dlsym(RTLD_DEFAULT, "towlower");
    ToWideFn glibcTowupper = (ToWideFn)dlsym(RTLD_DEFAULT, "towupper");
    IsNarrowFn glibcIsalpha = (IsNarrowFn)dlsym(RTLD_DEFAULT, "isalpha");
    IsNarrowFn glibcIsalnum = (IsNarrowFn)dlsym(RTLD_DEFAULT, "isalnum");
    IsNarrowFn glibcIsspace = (IsNarrowFn)dlsym(RTLD_DEFAULT, "isspace");
    IsNarrowFn glibcIsxdigit = (IsNarrowFn)dlsym(RTLD_DEFAULT, "isxdigit");
    ToNarrowFn glibcTolower = (ToNarrowFn)dlsym(RTLD_DEFAULT, "tolower");
    ToNarrowFn glibcToupper = (ToNarrowFn)dlsym(RTLD_DEFAULT, "toupper");
    WcscasecmpFn glibcWcscasecmp = (WcscasecmpFn)dlsym(RTLD_DEFAULT, "wcscasecmp");
    StrcasecmpFn glibcStrcasecmp = (StrcasecmpFn)dlsym(RTLD_DEFAULT, "strcasecmp");
    StrncasecmpFn glibcStrncasecmp = (StrncasecmpFn)dlsym(RTLD_DEFAULT, "strncasecmp");
    NewlocaleFn glibcNewlocale = (NewlocaleFn)dlsym(RTLD_DEFAULT, "newlocale");
    FreelocaleFn glibcFreelocale = (FreelocaleFn)dlsym(RTLD_DEFAULT, "freelocale");
    if (!glibcIswalpha || !glibcIswxdigit || !glibcTowlower || !glibcTowupper || !glibcIsalpha ||
        !glibcTolower || !glibcWcscasecmp || !glibcStrcasecmp || !glibcStrncasecmp || !glibcNewlocale ||
        !glibcFreelocale) {
        Expect(false, "glibc's wide ctype, byte fold and locale object functions are found");
        return;
    }

    // The battery lives where linux/smoke-test.sh put it; both libraries read LOCPATH, so both find
    // the same compiled locales and the comparison is of loaders, not of data.
    char savedLocpath[256];
    const char* previousLocpath = getenv("LOCPATH");
    savedLocpath[0] = 0;
    if (previousLocpath) snprintf(savedLocpath, sizeof savedLocpath, "%s", previousLocpath);
    if (const char* directory = getenv("CORDINAL_LOCALE_DIR"); directory && *directory)
        setenv("LOCPATH", directory, 1);

    const struct {
        const char* name;
        int (*mine)(wint_t);
        IsWideFn theirs;
    } kWide[] = {
        {"iswalnum", iswalnum, glibcIswalnum}, {"iswalpha", iswalpha, glibcIswalpha},
        {"iswblank", iswblank, glibcIswblank}, {"iswcntrl", iswcntrl, glibcIswcntrl},
        {"iswdigit", iswdigit, glibcIswdigit}, {"iswgraph", iswgraph, glibcIswgraph},
        {"iswlower", iswlower, glibcIswlower}, {"iswprint", iswprint, glibcIswprint},
        {"iswpunct", iswpunct, glibcIswpunct}, {"iswspace", iswspace, glibcIswspace},
        {"iswupper", iswupper, glibcIswupper}, {"iswxdigit", iswxdigit, glibcIswxdigit},
    };

    int compared = 0;
    int stripped = 0;
    int narrowOnly = 0;
    for (int locale = 0; locale < kLocaleBatteryCount; ++locale) {
        const char* name = kLocaleBattery[locale];
        if (!glibcSetlocale(LC_ALL, name)) {
            printf("skip: glibc has no %s, so the locale data checks leave it out\n", name);
            continue;
        }
        setlocale(LC_ALL, name);
        ++compared;
        char what[192];

        // Every LC_TIME item, as the data has it.
        int itemBad = 0;
        for (int item : kTimeLanginfo) {
            const char* mine = nl_langinfo(item);
            const char* theirs = glibcLanginfo(item);
            if (!mine || !theirs || strcmp(mine, theirs)) {
                if (itemBad < 4)
                    fprintf(stderr, "FAIL %s langinfo(%#x): [%s] against glibc's [%s]\n", name, item,
                            mine ? mine : "(null)", theirs ? theirs : "(null)");
                ++itemBad;
            }
        }
        snprintf(what, sizeof what, "%s: every LC_TIME langinfo item is the loaded data's", name);
        Expect(itemBad == 0, what);
        snprintf(what, sizeof what, "%s: the codeset is the loaded LC_CTYPE's", name);
        Expect(!strcmp(nl_langinfo(CODESET), glibcLanginfo(CODESET)), what);

        // strftime and wcsftime over the formats, in the locale's own words. A locale with eras or
        // alternative digits is compared against glibc's answer for the same format without the
        // modifier, which is what this layer promises there. wcsftime needs a UTF-8 codeset: this
        // layer's wide formatting formats narrowly and widens through UTF-8, so an 8-bit locale's
        // wide answers are its documented limit, not a comparison.
        const bool alternatives = HasAlternatives(glibcLanginfo);
        const bool utf8 = !strcmp(glibcLanginfo(CODESET), "UTF-8");
        if (!utf8) ++narrowOnly;
        int formatBad = 0;
        // March is in the list for the locales whose month name has a letter the byte tables fold
        // (de_DE's "März" in ISO-8859-1), which is what the ^ and # flags are compared on.
        time_t times[] = {-2208988800LL, 0, 1500000000, 1741219200LL, 253402300799LL};
        for (size_t i = 0; i < sizeof times / sizeof times[0]; ++i) {
            struct tm mine;
            if (!gmtime_r(&times[i], &mine)) continue;
            mine.tm_isdst = 0;
            mine.tm_gmtoff = 3600;
            mine.tm_zone = "UTC";
            struct tm theirs = mine;
            for (const char* format : kStrftimeFormats) {
                char strippedFormat[128];
                const char* theirFormat = format;
                if (alternatives) {
                    StripModifiers(format, strippedFormat, sizeof strippedFormat);
                    if (strcmp(strippedFormat, format)) ++stripped;
                    theirFormat = strippedFormat;
                }
                char mineText[512] = {}, theirText[512] = {};
                const size_t mineCount = strftime(mineText, sizeof mineText, format, &mine);
                const size_t theirCount = glibcStrftime(theirText, sizeof theirText, theirFormat, &theirs);
                // A count of zero means the result did not fit, and what a failed call left behind is
                // not defined: the counts are the whole comparison then.
                if (mineCount != theirCount || (mineCount && strcmp(mineText, theirText))) {
                    if (formatBad < 4)
                        fprintf(stderr, "FAIL %s strftime [%s] tm %zu: %zu [%s] against glibc's [%s] %zu [%s]\n",
                                name, format, i, mineCount, mineText, theirFormat, theirCount, theirText);
                    ++formatBad;
                }
                if (!utf8) continue;
                wchar_t mineWide[512] = {}, theirWide[512] = {}, wide[512];
                size_t length = 0;
                for (const char* p = format; *p; ++p) wide[length++] = (wchar_t)(unsigned char)*p;
                wide[length] = 0;
                wchar_t theirFormatWide[512];
                length = 0;
                for (const char* p = theirFormat; *p; ++p) theirFormatWide[length++] = (wchar_t)(unsigned char)*p;
                theirFormatWide[length] = 0;
                const size_t mineWideCount = wcsftime(mineWide, 512, wide, &mine);
                const size_t theirWideCount = glibcWcsftime(theirWide, 512, theirFormatWide, &theirs);
                if (mineWideCount != theirWideCount || (mineWideCount && wcscmp(mineWide, theirWide))) {
                    if (formatBad < 4) {
                        size_t at = 0;
                        while (mineWide[at] && mineWide[at] == theirWide[at]) ++at;
                        fprintf(stderr, "FAIL %s wcsftime [%s] tm %zu: %zu against glibc's %zu, first difference at %zu (%#x / %#x)\n",
                                name, format, i, mineWideCount, theirWideCount, at,
                                (unsigned)(at < 512 ? mineWide[at] : 0), (unsigned)(at < 512 ? theirWide[at] : 0));
                    }
                    ++formatBad;
                }
            }
        }
        snprintf(what, sizeof what, "%s: strftime%s are glibc's over %d formats", name,
                 utf8 ? " and wcsftime" : " (an 8-bit codeset: wcsftime is the documented limit)",
                 (int)(sizeof kStrftimeFormats / sizeof kStrftimeFormats[0]));
        Expect(formatBad == 0, what);

        // The narrow tables, every byte of both halves.
        int narrowBad = 0;
        for (int c = -128; c < 256; ++c) {
            const int mine[6] = {isalpha(c) != 0, isalnum(c) != 0, isspace(c) != 0, isxdigit(c) != 0, tolower(c),
                                 toupper(c)};
            const int theirs[6] = {glibcIsalpha(c) != 0, glibcIsalnum(c) != 0, glibcIsspace(c) != 0,
                                   glibcIsxdigit(c) != 0, glibcTolower(c), glibcToupper(c)};
            if (memcmp(mine, theirs, sizeof mine)) {
                if (narrowBad < 4)
                    fprintf(stderr, "FAIL %s narrow ctype %d: %d%d%d%d %d %d against glibc %d%d%d%d %d %d\n", name, c,
                            mine[0], mine[1], mine[2], mine[3], mine[4], mine[5], theirs[0], theirs[1], theirs[2],
                            theirs[3], theirs[4], theirs[5]);
                ++narrowBad;
            }
        }
        snprintf(what, sizeof what, "%s: the narrow class and case tables are the loaded data's", name);
        Expect(narrowBad == 0, what);

        // strcasecmp and strncasecmp fold each byte through that same table, as glibc's do. The pairs
        // include an 8-bit letter in both cases, which only a loaded 8-bit table makes equal.
        {
            static const struct {
                const char* left;
                const char* right;
            } kBytePairs[] = {{"abc", "ABC"},  {"abc", "abd"},       {"aBcD", "Abcx"},
                              {"\xe4rz", "\xc4RZ"}, {"\xc4RZ", "\xe4rz"}, {"\xdf", "\xdf"},
                              {"STRASSE", "strasse"}, {"\xff", "\xfe"},     {"", ""}};
            int foldBad = 0;
            for (const auto& pair : kBytePairs) {
                const int mine = strcasecmp(pair.left, pair.right);
                const int theirs = glibcStrcasecmp(pair.left, pair.right);
                if (mine != theirs || (mine == 0) != (theirs == 0)) {
                    if (foldBad < 4)
                        fprintf(stderr, "FAIL %s strcasecmp [%s] [%s]: %d against glibc %d\n", name, pair.left,
                                pair.right, mine, theirs);
                    ++foldBad;
                }
                for (size_t limit : {(size_t)0, (size_t)1, (size_t)2, (size_t)3, (size_t)4, (size_t)100}) {
                    if (strncasecmp(pair.left, pair.right, limit) != glibcStrncasecmp(pair.left, pair.right, limit)) {
                        if (foldBad < 4)
                            fprintf(stderr, "FAIL %s strncasecmp [%s] [%s] %zu\n", name, pair.left, pair.right, limit);
                        ++foldBad;
                    }
                }
            }
            snprintf(what, sizeof what, "%s: strcasecmp and strncasecmp fold through the loaded table", name);
            Expect(foldBad == 0, what);
            // And the fold really is the loaded one: in a locale whose table says 0xE4's upper case is
            // 0xC4, the pair compares equal through strcasecmp and differs two bytes in.
            if (!utf8 && toupper(0xE4) == 0xC4)
                Expect(strcasecmp("\xe4rz", "\xc4rz") == 0 && strcasecmp("\xe4rz", "\xc5rz") != 0,
                       "an 8-bit locale's accented letters fold through the loaded table");
        }

        // The wide classes and case maps over the whole Basic Multilingual Plane: the count of
        // disagreements is what a reader wants to see, and it has to be zero for loaded data.
        int worst = 0;
        const char* worstName = "";
        for (const auto& entry : kWide) {
            int bad = 0;
            for (unsigned wc = 0; wc <= 0xFFFF; ++wc) {
                if ((entry.mine((wint_t)wc) != 0) != (entry.theirs((wint_t)wc) != 0)) {
                    if (bad < 2)
                        fprintf(stderr, "FAIL %s %s(%#x): %d against glibc %d\n", name, entry.name, wc,
                                entry.mine((wint_t)wc), entry.theirs((wint_t)wc));
                    ++bad;
                }
            }
            if (bad > worst) {
                worst = bad;
                worstName = entry.name;
            }
        }
        for (int upper = 0; upper < 2; ++upper) {
            int bad = 0;
            for (unsigned wc = 0; wc <= 0xFFFF; ++wc) {
                const wint_t mine = upper ? towupper((wint_t)wc) : towlower((wint_t)wc);
                const wint_t theirs = upper ? glibcTowupper((wint_t)wc) : glibcTowlower((wint_t)wc);
                if (mine != theirs) {
                    if (bad < 2)
                        fprintf(stderr, "FAIL %s %s(%#x): %#x against glibc %#x\n", name,
                                upper ? "towupper" : "towlower", wc, mine, theirs);
                    ++bad;
                }
            }
            if (bad > worst) {
                worst = bad;
                worstName = upper ? "towupper" : "towlower";
            }
        }
        snprintf(what, sizeof what,
                 "%s: the wide classes and case maps match glibc's (worst %s, %d disagreements over the BMP)", name,
                 worstName, worst);
        Expect(worst == 0, what);

        // wcscasecmp folds through the same tables.
        {
            static const wchar_t* const kPairs[][2] = {{L"\u00c9clair", L"\u00e9clair"},
                                                       {L"I", L"\u0131"},
                                                       {L"i", L"\u0130"},
                                                       {L"\u01c5", L"\u01c6"},
                                                       {L"\u03a3", L"\u03c3"},
                                                       {L"abc", L"abd"},
                                                       {L"ABC", L"abc"}};
            int pairBad = 0;
            for (const auto& pair : kPairs) {
                const int mine = wcscasecmp(pair[0], pair[1]);
                const int theirs = glibcWcscasecmp(pair[0], pair[1]);
                if (mine != theirs) {
                    if (pairBad < 4)
                        fprintf(stderr, "FAIL %s wcscasecmp: %d against glibc %d\n", name, mine, theirs);
                    ++pairBad;
                }
            }
            snprintf(what, sizeof what, "%s: wcscasecmp folds as glibc's does", name);
            Expect(pairBad == 0, what);
        }

        if (!strcmp(name, "tr_TR.UTF-8")) {
            // Turkish's dotted and dotless I, which only the loaded data knows.
            Expect(towlower(L'I') == 0x131 && towlower(0x130) == L'i' && towupper(L'i') == 0x130 &&
                       towupper(0x131) == L'I',
                   "tr_TR: towlower turns I into the dotless i and towupper turns i into the dotted one");
            Expect(towlower(L'I') == glibcTowlower(L'I') && towlower(0x130) == glibcTowlower(0x130) &&
                       towupper(L'i') == glibcTowupper(L'i') && towupper(0x131) == glibcTowupper(0x131),
                   "tr_TR: glibc folds the dotted I the same way");
            Expect(wcscasecmp(L"I", L"\u0131") == 0 && wcscasecmp(L"i", L"\u0130") == 0,
                   "tr_TR: wcscasecmp pairs I with the dotless i and i with the dotted one");
            // A locale object of its own carries the same tables, which is what libstdc++ reads.
            locale_t object = newlocale(LC_ALL_MASK, name, (locale_t)0);
            Expect(object != (locale_t)0, "tr_TR: newlocale answers an object");
            if (object) {
                Expect(towlower_l(L'I', object) == 0x131 && towupper_l(L'i', object) == 0x130,
                       "tr_TR: a locale object folds the dotted I too");
                Expect(!strcmp(nl_langinfo_l(CODESET, object), "UTF-8"), "tr_TR: the object's codeset is UTF-8");
                void* previous = uselocale(object);
                Expect(towlower(L'I') == 0x131, "tr_TR: a uselocale'd object carries its locale's tables");
                uselocale((locale_t)previous);
                freelocale(object);
            }
            locale_t theirObject = glibcNewlocale ? (locale_t)glibcNewlocale(LC_ALL_MASK, name, (locale_t)0) : (locale_t)0;
            if (theirObject) glibcFreelocale(theirObject);
        }
    }
    if (!compared) {
        printf("skip: no locale of the battery is installed, so the locale data checks compare the fallback only\n");
    } else {
        printf("ok: %d locales compared with glibc's own data", compared);
        if (stripped) printf(" (%d E/O formats answered without the modifier)", stripped);
        if (narrowOnly) printf(" (%d 8-bit, compared narrow only)", narrowOnly);
        printf("\n");
    }

    // The C locale still has no letters above ASCII, and folds only theirs.
    {
        setlocale(LC_ALL, "C");
        glibcSetlocale(LC_ALL, "C");
        Expect(iswalpha(0xE9) == 0 && iswalnum(0x4E2D) == 0 && towlower(0xC9) == 0xC9,
               "the C locale has no letters above ASCII");
        Expect(!strcmp(nl_langinfo(CODESET), "ANSI_X3.4-1968") && __ctype_get_mb_cur_max() == 1,
               "the C locale's codeset and MB_CUR_MAX are glibc's");
    }

    // The fallback: a name nothing on this system has, so no data is found and the embedded Unicode
    // tables answer. glibc's C.UTF-8 is compiled from the same i18n definition, so it is the oracle
    // for the plane the tables cover.
    {
        setlocale(LC_ALL, "zz_ZZ.UTF-8");
        const bool oracle = glibcSetlocale(LC_ALL, "C.UTF-8") != nullptr;
        Expect(!strcmp(nl_langinfo(CODESET), "UTF-8") && __ctype_get_mb_cur_max() == 6,
               "the fallback keeps the codeset its name asks for");
        Expect(iswalpha(0xE9) && towlower(0xC9) == 0xE9 && towupper(0xE9) == 0xC9 && iswalpha(0x660) &&
                   iswspace(0x3000) && iswalpha(0x4E2D) && !iswalpha(0x20),
               "the fallback classifies and folds beyond ASCII");
        // The tables cover the Basic Multilingual Plane; above it a code point is in no class and maps
        // to itself, which is the fallback's documented limit.
        Expect(!iswalpha(0x10400) && towlower(0x10400) == 0x10400, "the fallback stops at the BMP");
        if (oracle) {
            int worst = 0;
            const char* worstName = "";
            for (const auto& entry : kWide) {
                int bad = 0;
                for (unsigned wc = 0; wc <= 0xFFFF; ++wc)
                    if ((entry.mine((wint_t)wc) != 0) != (entry.theirs((wint_t)wc) != 0)) ++bad;
                if (bad > worst) {
                    worst = bad;
                    worstName = entry.name;
                }
            }
            for (int upper = 0; upper < 2; ++upper) {
                int bad = 0;
                for (unsigned wc = 0; wc <= 0xFFFF; ++wc) {
                    const wint_t mine = upper ? towupper((wint_t)wc) : towlower((wint_t)wc);
                    const wint_t theirs = upper ? glibcTowupper((wint_t)wc) : glibcTowlower((wint_t)wc);
                    if (mine != theirs) ++bad;
                }
                if (bad > worst) {
                    worst = bad;
                    worstName = upper ? "towupper" : "towlower";
                }
            }
            char what[192];
            snprintf(what, sizeof what,
                     "the fallback matches glibc's C.UTF-8 over the BMP (worst %s, %d disagreements)", worstName,
                     worst);
            Expect(worst == 0, what);
        } else {
            printf("skip: glibc has no C.UTF-8, so the fallback's tables are not compared with it\n");
        }
    }

    setlocale(LC_ALL, "C");
    glibcSetlocale(LC_ALL, "C");
    if (previousLocpath) setenv("LOCPATH", savedLocpath, 1);
    else unsetenv("LOCPATH");
}



inline void CheckLocale() {
    using namespace locale_glibc;
    auto glibcSetlocale = (SetlocaleFn)dlsym(RTLD_DEFAULT, "setlocale");
    auto glibcLocaleconv = (LocaleconvFn)dlsym(RTLD_DEFAULT, "localeconv");
    auto glibcLanginfo = (NlLanginfoFn)dlsym(RTLD_DEFAULT, "nl_langinfo");
    auto glibcStrftime = (StrftimeFn)dlsym(RTLD_DEFAULT, "strftime");
    auto glibcWcsftime = (WcsftimeFn)dlsym(RTLD_DEFAULT, "wcsftime");
    auto glibcIsalpha = (IsalphaFn)dlsym(RTLD_DEFAULT, "isalpha");
    auto glibcIsalnum = (IsalnumFn)dlsym(RTLD_DEFAULT, "isalnum");
    auto glibcIsspace = (IsspaceFn)dlsym(RTLD_DEFAULT, "isspace");
    auto glibcIsxdigit = (IsxdigitFn)dlsym(RTLD_DEFAULT, "isxdigit");
    auto glibcTolower = (TolowerFn)dlsym(RTLD_DEFAULT, "tolower");
    auto glibcToupper = (ToupperFn)dlsym(RTLD_DEFAULT, "toupper");
    auto glibcIswalnum = (IswalnumFn)dlsym(RTLD_DEFAULT, "iswalnum");
    auto glibcIswalpha = (IswalphaFn)dlsym(RTLD_DEFAULT, "iswalpha");
    auto glibcIswspace = (IswspaceFn)dlsym(RTLD_DEFAULT, "iswspace");
    auto glibcIswxdigit = (IswxdigitFn)dlsym(RTLD_DEFAULT, "iswxdigit");
    auto glibcTowlower = (TowlowerFn)dlsym(RTLD_DEFAULT, "towlower");
    auto glibcTowupper = (TowupperFn)dlsym(RTLD_DEFAULT, "towupper");
    auto glibcMbrtowc = (MbrtowcFn)dlsym(RTLD_DEFAULT, "mbrtowc");
    auto glibcWcrtomb = (WcrtombFn)dlsym(RTLD_DEFAULT, "wcrtomb");
    auto glibcBtowc = (BtowcFn)dlsym(RTLD_DEFAULT, "btowc");
    auto glibcWctob = (WctobFn)dlsym(RTLD_DEFAULT, "wctob");
    auto glibcMbsrtowcs = (MbsrtowcsFn)dlsym(RTLD_DEFAULT, "mbsrtowcs");
    auto glibcWcsrtombs = (WcsrtombsFn)dlsym(RTLD_DEFAULT, "wcsrtombs");
    auto glibcMbCurMax = (CtypeMbCurMaxFn)dlsym(RTLD_DEFAULT, "__ctype_get_mb_cur_max");
    auto glibcStrxfrm = (StrxfrmFn)dlsym(RTLD_DEFAULT, "strxfrm");
    auto glibcStrcoll = (StrcollFn)dlsym(RTLD_DEFAULT, "strcoll");
    if (!glibcSetlocale || !glibcLocaleconv || !glibcLanginfo || !glibcStrftime || !glibcWcsftime ||
        !glibcIsalpha || !glibcMbrtowc || !glibcMbCurMax) {
        Expect(false, "glibc's locale functions are found");
        return;
    }
    Expect(InProgram((const void*)&setlocale) && InProgram((const void*)&localeconv) &&
               InProgram((const void*)&nl_langinfo) && InProgram((const void*)&strftime) &&
               InProgram((const void*)&isalpha) && InProgram((const void*)&mbrtowc) &&
               InProgram((const void*)&newlocale) && !InProgram((const void*)glibcSetlocale),
           "setlocale, localeconv, nl_langinfo, strftime, isalpha, mbrtowc and newlocale are cordinal's");

    // Both libraries in the C locale, which is the data this layer serves.
    setlocale(LC_ALL, "C");
    glibcSetlocale(LC_ALL, "C");

    // struct lconv: every field is glibc's, read through glibc's own type.
    {
        const struct lconv* ours = localeconv();
        const struct lconv* theirs = glibcLocaleconv();
        Expect(ours && theirs, "localeconv answers");
        if (ours && theirs) {
            Expect(!strcmp(ours->decimal_point, theirs->decimal_point) &&
                       !strcmp(ours->thousands_sep, theirs->thousands_sep) &&
                       !strcmp(ours->grouping, theirs->grouping) &&
                       !strcmp(ours->int_curr_symbol, theirs->int_curr_symbol) &&
                       !strcmp(ours->currency_symbol, theirs->currency_symbol) &&
                       !strcmp(ours->mon_decimal_point, theirs->mon_decimal_point) &&
                       !strcmp(ours->mon_thousands_sep, theirs->mon_thousands_sep) &&
                       !strcmp(ours->mon_grouping, theirs->mon_grouping) &&
                       !strcmp(ours->positive_sign, theirs->positive_sign) &&
                       !strcmp(ours->negative_sign, theirs->negative_sign),
                   "localeconv's strings are glibc's");
            Expect(ours->int_frac_digits == theirs->int_frac_digits && ours->frac_digits == theirs->frac_digits &&
                       ours->p_cs_precedes == theirs->p_cs_precedes &&
                       ours->p_sep_by_space == theirs->p_sep_by_space &&
                       ours->n_cs_precedes == theirs->n_cs_precedes &&
                       ours->n_sep_by_space == theirs->n_sep_by_space &&
                       ours->p_sign_posn == theirs->p_sign_posn && ours->n_sign_posn == theirs->n_sign_posn &&
                       ours->int_p_cs_precedes == theirs->int_p_cs_precedes &&
                       ours->int_p_sep_by_space == theirs->int_p_sep_by_space &&
                       ours->int_n_cs_precedes == theirs->int_n_cs_precedes &&
                       ours->int_n_sep_by_space == theirs->int_n_sep_by_space &&
                       ours->int_p_sign_posn == theirs->int_p_sign_posn &&
                       ours->int_n_sign_posn == theirs->int_n_sign_posn,
                   "localeconv's numeric fields are glibc's");
            if (failures) fprintf(stderr, "  decimal point [%s] thousands [%s]\n", ours->decimal_point, ours->thousands_sep);
        }
    }

    // nl_langinfo: the C locale's strings, item by item.
    {
        static const int kItems[] = {CODESET,  RADIXCHAR, THOUSEP,   CRNCYSTR,  YESEXPR,  NOEXPR,
                                     YESSTR,  NOSTR,     D_T_FMT,   D_FMT,     T_FMT,    T_FMT_AMPM,
                                     AM_STR,  PM_STR,    ABDAY_1,   ABDAY_7,   DAY_1,    DAY_7,
                                     ABMON_1, ABMON_12,  MON_1,     MON_12,    ERA,      ERA_D_FMT,
                                     ERA_D_T_FMT, ERA_T_FMT, ALT_DIGITS};
        for (int item : kItems) {
            const char* mine = nl_langinfo(item);
            const char* theirs = glibcLanginfo(item);
            if (!mine || !theirs || strcmp(mine, theirs)) {
                if (failures < 40)
                    fprintf(stderr, "FAIL nl_langinfo(%d): [%s] against glibc's [%s]\n", item, mine ? mine : "(null)",
                            theirs ? theirs : "(null)");
                ++failures;
            }
        }
        Expect(!strcmp(nl_langinfo(CODESET), "ANSI_X3.4-1968"), "the C locale's codeset is ASCII");
        Expect(__ctype_get_mb_cur_max() == 1, "MB_CUR_MAX is 1 in the C locale");
        Expect(glibcMbCurMax() == 1, "glibc agrees about MB_CUR_MAX in the C locale");
    }

    // The narrow character classes and case maps, every byte of glibc's table halves.
    {
        int bad = 0;
        for (int c = -128; c < 256; ++c) {
            const int mine[6] = {isalpha(c) != 0, isalnum(c) != 0, isspace(c) != 0, isxdigit(c) != 0, tolower(c),
                                 toupper(c)};
            const int theirs[6] = {glibcIsalpha(c) != 0, glibcIsalnum(c) != 0, glibcIsspace(c) != 0,
                                   glibcIsxdigit(c) != 0, glibcTolower(c), glibcToupper(c)};
            if (memcmp(mine, theirs, sizeof mine)) {
                if (bad++ < 8)
                    fprintf(stderr, "FAIL ctype %d: %d%d%d%d %d %d against glibc %d%d%d%d %d %d\n", c, mine[0],
                            mine[1], mine[2], mine[3], mine[4], mine[5], theirs[0], theirs[1], theirs[2], theirs[3],
                            theirs[4], theirs[5]);
            }
        }
        Expect(bad == 0, "the narrow character classes and case maps match glibc's C locale");
        Expect(tolower(EOF) == EOF && toupper(EOF) == EOF && isalpha(EOF) == 0, "EOF passes through ctype");
        Expect(isalpha(1000) == 0 && tolower(1000) == 1000, "a value outside the table is left alone");
    }

    // The wide classes and case maps: the C locale has no letters above ASCII.
    {
        int bad = 0;
        for (int c = 0; c < 0x3000; ++c) {
            const int mine[6] = {iswalnum(c) != 0, iswalpha(c) != 0, iswspace(c) != 0, iswxdigit(c) != 0,
                                 (int)towlower(c), (int)towupper(c)};
            const int theirs[6] = {glibcIswalnum(c) != 0, glibcIswalpha(c) != 0, glibcIswspace(c) != 0,
                                   glibcIswxdigit(c) != 0, (int)glibcTowlower(c), (int)glibcTowupper(c)};
            if (memcmp(mine, theirs, sizeof mine)) {
                if (bad++ < 8)
                    fprintf(stderr, "FAIL wide ctype %x: %d%d%d%d %x %x against glibc %d%d%d%d %x %x\n", c, mine[0],
                            mine[1], mine[2], mine[3], mine[4], mine[5], theirs[0], theirs[1], theirs[2], theirs[3],
                            theirs[4], theirs[5]);
            }
        }
        Expect(bad == 0, "the wide character classes and case maps match glibc's C locale");
        Expect(iswctype(L'a', wctype("alpha")) != 0 && iswctype(L'1', wctype("digit")) != 0 &&
                   iswctype(L'7', wctype("alpha")) == 0 && wctype("bogus") == 0,
               "wctype and iswctype know the C locale's classes");
    }

    // The multibyte conversions, in the C locale and (when glibc has it) in C.UTF-8.
    {
        static const char* const kBytes[] = {"a",      "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80",
                                             "\xc3",   "\x80",     "\xc0\x80",     "\xed\xa0\x80",
                                             "\xf4\x90\x80\x80", "\xf8\x88\x80\x80\x80", "\xfe"};
        static const size_t kLengths[] = {1, 2, 3, 4, 1, 1, 2, 3, 4, 5, 1};
        for (int round = 0; round < 2; ++round) {
            const bool utf8 = round == 1;
            if (utf8) {
                if (!glibcSetlocale(LC_ALL, "C.UTF-8")) continue;  // an old glibc without C.UTF-8
                setlocale(LC_ALL, "C.UTF-8");
            } else {
                setlocale(LC_ALL, "C");
                glibcSetlocale(LC_ALL, "C");
            }
            Expect(__ctype_get_mb_cur_max() == (utf8 ? (size_t)6 : (size_t)1), "MB_CUR_MAX follows the codeset");
            Expect(glibcMbCurMax() == __ctype_get_mb_cur_max(), "glibc's MB_CUR_MAX agrees");
            Expect(!strcmp(nl_langinfo(CODESET), utf8 ? "UTF-8" : "ANSI_X3.4-1968"), "the codeset is named");
            for (size_t i = 0; i < sizeof kBytes / sizeof kBytes[0]; ++i) {
                mbstate_t mine{}, theirs{};
                wchar_t mineWide = 0, theirWide = 0;
                errno = 0;
                const size_t mineCount = mbrtowc(&mineWide, kBytes[i], kLengths[i], &mine);
                const int mineErrno = errno;
                errno = 0;
                const size_t theirCount = glibcMbrtowc(&theirWide, kBytes[i], kLengths[i], &theirs);
                if (mineCount != theirCount || mineWide != theirWide || mineErrno != errno) {
                    if (failures < 40)
                        fprintf(stderr, "FAIL mbrtowc [%02x..] %zu bytes: %zd %x errno %d, glibc %zd %x errno %d\n",
                                (unsigned char)kBytes[i][0], kLengths[i], mineCount, (unsigned)mineWide, mineErrno,
                                theirCount, (unsigned)theirWide, errno);
                    ++failures;
                }
            }
            // Split sequences: the answer is glibc's for each piece.
            {
                mbstate_t mine{}, theirs{};
                wchar_t mineWide = 0, theirWide = 0;
                errno = 0;
                const size_t mineFirst = mbrtowc(&mineWide, "\xe2\x82", 2, &mine);
                const int mineErrno = errno;
                errno = 0;
                const size_t theirFirst = glibcMbrtowc(&theirWide, "\xe2\x82", 2, &theirs);
                const int theirErrno = errno;
                errno = 0;
                const size_t mineSecond = mbrtowc(&mineWide, "\xac", 1, &mine);
                const int mineSecondErrno = errno;
                errno = 0;
                const size_t theirSecond = glibcMbrtowc(&theirWide, "\xac", 1, &theirs);
                if (mineFirst != theirFirst || mineErrno != theirErrno || mineSecond != theirSecond ||
                    mineSecondErrno != errno || mineWide != theirWide) {
                    if (failures < 40)
                        fprintf(stderr, "FAIL split mbrtowc: %zd/%zd then %zd/%zd, glibc %zd/%zd then %zd/%zd\n",
                                mineFirst, mineSecond, mineWide, theirWide, theirFirst, theirSecond, theirWide,
                                theirWide);
                    ++failures;
                }
            }
            static const wchar_t kWide[] = {L'a', 0xE9, 0x20AC, 0x1F600, 0xD800, 0xFFFD, 0x110000};
            for (wchar_t wc : kWide) {
                char mineBytes[16] = {}, theirBytes[16] = {};
                mbstate_t mine{}, theirs{};
                errno = 0;
                const size_t mineCount = wcrtomb(mineBytes, wc, &mine);
                const int mineErrno = errno;
                errno = 0;
                const size_t theirCount = glibcWcrtomb(theirBytes, wc, &theirs);
                if (mineCount != theirCount || mineErrno != errno ||
                    (mineCount != (size_t)-1 && memcmp(mineBytes, theirBytes, mineCount))) {
                    if (failures < 40)
                        fprintf(stderr,
                                "FAIL wcrtomb %x: %zd errno %d [%02x%02x%02x%02x] against glibc %zd errno %d "
                                "[%02x%02x%02x%02x]\n",
                                (unsigned)wc, mineCount, mineErrno, (unsigned char)mineBytes[0],
                                (unsigned char)mineBytes[1], (unsigned char)mineBytes[2], (unsigned char)mineBytes[3],
                                theirCount, errno, (unsigned char)theirBytes[0], (unsigned char)theirBytes[1],
                                (unsigned char)theirBytes[2], (unsigned char)theirBytes[3]);
                    ++failures;
                }
            }
            for (int c = -1; c < 0x120; ++c) {
                if (btowc(c) != glibcBtowc(c)) {
                    if (failures < 40) fprintf(stderr, "FAIL btowc %d\n", c);
                    ++failures;
                }
                if (wctob((wint_t)c) != glibcWctob((wint_t)c)) {
                    if (failures < 40) fprintf(stderr, "FAIL wctob %d\n", c);
                    ++failures;
                }
            }
            // Whole strings, both ways, including a buffer that is too small.
            static const char* const kStrings[] = {"", "abc", "a\xc3\xa9z", "\xe2\x82\xac"};
            static const size_t kRooms[] = {0, 1, 4, 8};
            for (const char* text : kStrings) {
                for (size_t room : kRooms) {
                    wchar_t mineWide[8] = {}, theirWide[8] = {};
                    const char* mineAt = text;
                    const char* theirAt = text;
                    errno = 0;
                    const size_t mineCount = mbsrtowcs(mineWide, &mineAt, room, nullptr);
                    const int mineErrno = errno;
                    errno = 0;
                    const size_t theirCount = glibcMbsrtowcs(theirWide, &theirAt, room, nullptr);
                    if (mineCount != theirCount || mineErrno != errno ||
                        (mineCount != (size_t)-1 && memcmp(mineWide, theirWide, (mineCount + 1) * sizeof(wchar_t)))) {
                        if (failures < 40)
                            fprintf(stderr, "FAIL mbsrtowcs [%s] room %zu: %zd errno %d against glibc %zd errno %d\n",
                                    text, room, mineCount, mineErrno, theirCount, errno);
                        ++failures;
                    }
                    char mineBytes[16] = {}, theirBytes[16] = {};
                    const wchar_t* mineSource = mineWide;
                    const wchar_t* theirSource = theirWide;
                    const size_t mineBytesCount = wcsrtombs(mineBytes, &mineSource, room, nullptr);
                    const size_t theirBytesCount = glibcWcsrtombs(theirBytes, &theirSource, room, nullptr);
                    if (mineBytesCount != theirBytesCount ||
                        (mineBytesCount != (size_t)-1 && memcmp(mineBytes, theirBytes, mineBytesCount))) {
                        if (failures < 40)
                            fprintf(stderr, "FAIL wcsrtombs room %zu: %zd against glibc %zd\n", room, mineBytesCount,
                                    theirBytesCount);
                        ++failures;
                    }
                }
            }
        }
        setlocale(LC_ALL, "C");
        glibcSetlocale(LC_ALL, "C");
    }

    // The C locale's collation.
    {
        char mine[8], theirs[8];
        Expect(__strcoll_l("abc", "abd", nullptr) < 0 && __strcoll_l("abc", "abc", nullptr) == 0,
               "__strcoll_l orders bytes");
        Expect(__strxfrm_l(mine, "abc", sizeof mine, nullptr) == 3 && !strcmp(mine, "abc") &&
                   glibcStrxfrm(theirs, "abc", sizeof theirs) == 3 && !strcmp(mine, theirs),
               "__strxfrm_l copies as glibc's does");
        Expect(__wcscoll_l(L"abc", L"abd", nullptr) < 0 && glibcStrcoll("abc", "abd") < 0,
               "__wcscoll_l orders wide values");
        wchar_t wide[8];
        Expect(__wcsxfrm_l(wide, L"abc", 8, nullptr) == 3 && !wcscmp(wide, L"abc"), "__wcsxfrm_l copies");
    }

    // setlocale's names and the compound it builds for mixed categories.
    {
        Expect(!strcmp(setlocale(LC_ALL, "C"), "C"), "setlocale(LC_ALL, \"C\") answers C");
        Expect(!strcmp(setlocale(LC_ALL, "POSIX"), "C"), "POSIX is the C locale's other name");
        Expect(!strcmp(glibcSetlocale(LC_ALL, "POSIX"), "C"), "glibc agrees about POSIX");
        Expect(setlocale(99, "C") == nullptr, "a category that is not one is refused");
        setlocale(LC_ALL, "C");
        glibcSetlocale(LC_ALL, "C");
        setlocale(LC_NUMERIC, "C.UTF-8");
        glibcSetlocale(LC_NUMERIC, "C.UTF-8");
        const char* mine = setlocale(LC_ALL, nullptr);
        const char* theirs = glibcSetlocale(LC_ALL, nullptr);
        Expect(mine && theirs && !strcmp(mine, theirs), "the mixed-category compound is glibc's");
        if (mine && theirs && strcmp(mine, theirs)) fprintf(stderr, "  [%s] against glibc [%s]\n", mine, theirs);
        setlocale(LC_ALL, "C");
        glibcSetlocale(LC_ALL, "C");
        Expect(setlocale(LC_CTYPE, nullptr) && !strcmp(setlocale(LC_CTYPE, nullptr), "C"),
               "LC_CTYPE's own name is C");
        // "" reads the environment; cordinal's own name is accepted whatever it says.
        Expect(setlocale(LC_ALL, "") != nullptr, "setlocale(LC_ALL, \"\") answers a name");
        setlocale(LC_ALL, "C");
        // A locale object: what libstdc++ builds.
        locale_t object = newlocale(LC_CTYPE_MASK, "", (locale_t)0);
        Expect(object != (locale_t)0, "newlocale(LC_CTYPE_MASK, \"\") answers an object");
        if (object) {
            const char* codeset = nl_langinfo_l(CODESET, object);
            Expect(codeset && (!strcmp(codeset, "UTF-8") || !strcmp(codeset, "ANSI_X3.4-1968")),
                   "a locale object names its codeset");
            locale_t copy = duplocale(object);
            Expect(copy != (locale_t)0, "duplocale answers an object");
            if (copy) freelocale(copy);
            freelocale(object);
        }
        locale_t ascii = newlocale(LC_ALL_MASK, "C", (locale_t)0);
        Expect(ascii && !strcmp(nl_langinfo_l(CODESET, ascii), "ANSI_X3.4-1968"), "the C object's codeset is ASCII");
        if (ascii) freelocale(ascii);
        errno = 0;
        Expect(newlocale(1 << 30, "C", (locale_t)0) == nullptr && errno == EINVAL, "a mask outside the categories is EINVAL");
        Expect(uselocale((locale_t)0) == LC_GLOBAL_LOCALE, "the thread starts on the global locale");
    }

    // strftime and wcsftime, byte for byte.
    {
        time_t times[] = {-2208988800LL, -1, 0, 86399, 1500000000, 2147483647, 253402300799LL};
        int tms = 0;
        for (size_t i = 0; i < sizeof times / sizeof times[0]; ++i) {
            struct tm mine;
            if (!gmtime_r(&times[i], &mine)) continue;
            struct tm theirs = mine;
            // Both DST states and a few unusual zone fields, which %Z and %z read.
            for (int variant = 0; variant < 4; ++variant) {
                mine.tm_isdst = variant == 0 ? 0 : variant == 1 ? 1 : -1;
                mine.tm_gmtoff = variant == 2 ? -18000 : variant == 3 ? 20700 : 3600;
                mine.tm_zone = variant == 3 ? "ABCD" : variant == 2 ? "" : "UTC";
                theirs = mine;
                ++tms;
                for (const char* format : kStrftimeFormats) {
                    char mineText[256] = {}, theirText[256] = {};
                    const size_t mineCount = strftime(mineText, sizeof mineText, format, &mine);
                    const size_t theirCount = glibcStrftime(theirText, sizeof theirText, format, &theirs);
                    if (mineCount != theirCount || strcmp(mineText, theirText)) {
                        if (failures < 40)
                            fprintf(stderr, "FAIL strftime [%s] tm %d: %zu [%s] against glibc %zu [%s]\n", format, tms,
                                    mineCount, mineText, theirCount, theirText);
                        ++failures;
                    }
                }
                for (const char* format : kStrftimeFormats) {
                    wchar_t mineText[256] = {}, theirText[256] = {};
                    wchar_t wide[256];
                    size_t length = 0;
                    for (const char* p = format; *p; ++p) wide[length++] = (wchar_t)(unsigned char)*p;
                    wide[length] = 0;
                    const size_t mineCount = wcsftime(mineText, 256, wide, &mine);
                    const size_t theirCount = glibcWcsftime(theirText, 256, wide, &theirs);
                    if (mineCount != theirCount || wcscmp(mineText, theirText)) {
                        if (failures < 40)
                            fprintf(stderr, "FAIL wcsftime [%s] tm %d: %zu against glibc %zu\n", format, tms,
                                    mineCount, theirCount);
                        ++failures;
                    }
                }
            }
        }
        // Truncation: glibc writes what fits and answers 0, and so does this.
        struct tm tm;
        time_t now = 1500000000;
        gmtime_r(&now, &tm);
        tm.tm_zone = "UTC";
        tm.tm_gmtoff = 0;
        tm.tm_isdst = 0;
        static const struct {
            const char* format;
            size_t size;
        } kTruncations[] = {{"abcd", 3}, {"abcd", 4}, {"abcd", 5}, {"ab%Y", 3},
                           {"ab%Y", 7}, {"%Y%Y", 5}, {"%Y%Y", 9}, {"%Y-%m-%d", 10}, {"%Y-%m-%d", 11}, {"", 1},
                           {"abc", 0}};
        for (const auto& truncation : kTruncations) {
            char mineText[16], theirText[16];
            memset(mineText, 'x', sizeof mineText);
            memset(theirText, 'x', sizeof theirText);
            const size_t mineCount = strftime(mineText, truncation.size, truncation.format, &tm);
            const size_t theirCount = glibcStrftime(theirText, truncation.size, truncation.format, &tm);
            if (mineCount != theirCount || memcmp(mineText, theirText, sizeof mineText)) {
                if (failures < 40)
                    fprintf(stderr, "FAIL strftime truncation [%s] size %zu: %zu [%.*s] against glibc %zu [%.*s]\n",
                            truncation.format, truncation.size, mineCount, (int)sizeof mineText, mineText,
                            theirCount, (int)sizeof theirText, theirText);
                ++failures;
            }
        }
        // The extremes glibc's own answers pin down: a negative year and a far one.
        struct tm extreme;
        memset(&extreme, 0, sizeof extreme);
        extreme.tm_year = -1900;
        extreme.tm_mday = 1;
        extreme.tm_zone = "X";
        char mineText[64], theirText[64];
        const size_t mineCount = strftime(mineText, sizeof mineText, "%Y %C %y %j %s %G %g", &extreme);
        const size_t theirCount = glibcStrftime(theirText, sizeof theirText, "%Y %C %y %j %s %G %g", &extreme);
        Expect(mineCount == theirCount && !strcmp(mineText, theirText), "a year before 1900 formats as glibc's");
        if (mineCount != theirCount || strcmp(mineText, theirText))
            fprintf(stderr, "  [%s] against glibc [%s]\n", mineText, theirText);
        extreme.tm_year = 8000;
        const size_t mineFar = strftime(mineText, sizeof mineText, "%Y %C %y %s", &extreme);
        const size_t theirFar = glibcStrftime(theirText, sizeof theirText, "%Y %C %y %s", &extreme);
        Expect(mineFar == theirFar && !strcmp(mineText, theirText), "a far year formats as glibc's");
        if (mineFar != theirFar || strcmp(mineText, theirText))
            fprintf(stderr, "  [%s] against glibc [%s]\n", mineText, theirText);
    }

    // wcscasecmp folds ASCII, as glibc's does in the C locale.
    Expect(wcscasecmp(L"AbC", L"aBc") == 0 && wcscasecmp(L"abc", L"abd") < 0 &&
               wcsncasecmp(L"aBcD", L"Abcx", 3) == 0 && wcsncasecmp(L"aBcD", L"Abcx", 4) != 0,
           "wcscasecmp and wcsncasecmp fold ASCII");

    CheckLocaleData(glibcSetlocale, glibcLanginfo, glibcStrftime, glibcWcsftime);
}

}  // namespace libc_checks
