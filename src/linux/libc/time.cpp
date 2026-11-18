// cordinal's own clocks and calendar (stage 3 of replacing glibc): clock_gettime, gettimeofday and
// time through the kernel's vDSO, gmtime_r, timegm, difftime, and local time (localtime_r,
// mktime, tzset) from the system's time-zone files (TZif, RFC 8536) and POSIX TZ strings. Hidden
// like kernel.cpp: cordinal and the static libstdc++ use these, glibc keeps its own.
// Local time follows glibc's choices wherever they are its own: the TZ variable's forms, the type
// used before a zone's first transition, the footer rule after its last, and how mktime reads a
// tm_isdst that disagrees with the date. `cordinal-core-test libc` compares them with glibc's.
// strftime waited for the locale stage and is cordinal's own since stage 5b (locale.cpp), which also
// reads the zone's name through cordinal_zone_name below. Leap-second zones ("right/...") are read
// without their leap seconds.
#include "syscall.h"
#include <elf.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include <new>

namespace {
// The vDSO: the kernel's own clock code mapped into every process, which reads the clocks
// without entering the kernel. Its address arrives in the auxiliary vector, which follows the
// environment on the initial stack; a constructor finds it there (before it runs, or without a
// vDSO, the clocks are system calls).
typedef long (*VdsoClock)(long, void*);
typedef long (*VdsoTimeOfDay)(void*, void*);
typedef long (*VdsoTime)(long*);
VdsoClock vdsoClock;
VdsoTimeOfDay vdsoTimeOfDay;
VdsoTime vdsoTime;

void ReadVdso(uintptr_t base) {
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)base;
    if (!base || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64) return;
    const Elf64_Phdr* ph = (const Elf64_Phdr*)(base + eh->e_phoff);
    intptr_t bias = 0;
    bool loaded = false;
    const Elf64_Dyn* dynamic = nullptr;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_LOAD && !loaded) {
            bias = (intptr_t)base + (intptr_t)ph[i].p_offset - (intptr_t)ph[i].p_vaddr;
            loaded = true;
        } else if (ph[i].p_type == PT_DYNAMIC) {
            dynamic = (const Elf64_Dyn*)(base + ph[i].p_offset);
        }
    }
    if (!loaded || !dynamic) return;
    const Elf64_Sym* symbols = nullptr;
    const char* names = nullptr;
    const uint32_t* hash = nullptr;
    const uint32_t* gnuHash = nullptr;
    for (const Elf64_Dyn* d = dynamic; d->d_tag != DT_NULL; ++d) {
        const uintptr_t at = (uintptr_t)(d->d_un.d_ptr + bias);
        if (d->d_tag == DT_SYMTAB) symbols = (const Elf64_Sym*)at;
        else if (d->d_tag == DT_STRTAB) names = (const char*)at;
        else if (d->d_tag == DT_HASH) hash = (const uint32_t*)at;
        else if (d->d_tag == DT_GNU_HASH) gnuHash = (const uint32_t*)at;
    }
    if (!symbols || !names || (!hash && !gnuHash)) return;
    // The number of symbols: the SysV hash table states it; the GNU one ends at the last chain.
    size_t count = 0;
    if (hash) {
        count = hash[1];
    } else {
        const uint32_t buckets = gnuHash[0], offset = gnuHash[1], bloomWords = gnuHash[2];
        const uint32_t* bucket = gnuHash + 4 + bloomWords * 2;
        const uint32_t* chain = bucket + buckets;
        uint32_t last = 0;
        for (uint32_t i = 0; i < buckets; ++i)
            if (bucket[i] > last) last = bucket[i];
        if (last >= offset)
            while (!(chain[last - offset] & 1)) ++last;
        count = last >= offset ? last + 1 : offset;
    }
    for (size_t i = 0; i < count; ++i) {
        const Elf64_Sym& s = symbols[i];
        if (s.st_shndx == SHN_UNDEF || ELF64_ST_TYPE(s.st_info) != STT_FUNC) continue;
        const char* name = names + s.st_name;
        void* address = (void*)(s.st_value + bias);
        if (!strcmp(name, "__vdso_clock_gettime")) vdsoClock = (VdsoClock)address;
        else if (!strcmp(name, "__vdso_gettimeofday")) vdsoTimeOfDay = (VdsoTimeOfDay)address;
        else if (!strcmp(name, "__vdso_time")) vdsoTime = (VdsoTime)address;
    }
}
// Constructors get the program's original environment; the auxiliary vector follows its end.
__attribute__((constructor(101))) void FindVdso(int, char**, char** environment) {
    if (!environment) return;
    char** p = environment;
    while (*p) ++p;
    for (const unsigned long* aux = (const unsigned long*)(p + 1); aux[0]; aux += 2)
        if (aux[0] == AT_SYSINFO_EHDR) ReadVdso(aux[1]);
}

// The calendar: days since 1970-01-01 and back, for any year (Howard Hinnant's algorithms).
int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
void CivilFromDays(int64_t z, int64_t& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = yoe + era * 400 + (m <= 2);
}
inline int64_t FloorDiv(int64_t a, int64_t b) { return a / b - (a % b != 0 && (a % b < 0) != (b < 0)); }
inline bool Leap(int64_t y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

// Fills tm for time t at the given offset; false (EOVERFLOW) if the year doesn't fit an int.
bool BreakDown(int64_t t, long offset, int isdst, const char* zone, tm* out) {
    int64_t seconds;
    if (__builtin_add_overflow(t, (int64_t)offset, &seconds)) return false;
    const int64_t days = FloorDiv(seconds, 86400);
    const int64_t rest = seconds - days * 86400;
    int64_t year;
    int month, day;
    CivilFromDays(days, year, month, day);
    if (year - 1900 > INT32_MAX || year - 1900 < INT32_MIN) return false;
    out->tm_sec = (int)(rest % 60);
    out->tm_min = (int)(rest / 60 % 60);
    out->tm_hour = (int)(rest / 3600);
    out->tm_mday = day;
    out->tm_mon = month - 1;
    out->tm_year = (int)(year - 1900);
    out->tm_wday = (int)(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday (4)
    out->tm_yday = (int)(days - DaysFromCivil(year, 1, 1));
    out->tm_isdst = isdst;
    out->tm_gmtoff = offset;
    out->tm_zone = zone;
    return true;
}
// A tm's fields as seconds counted as if every day were UTC, with every field out of range
// carried into the next (as mktime and timegm do). False if that overflows.
bool LocalSeconds(const tm* in, int64_t& out) {
    int64_t month = in->tm_mon, year = (int64_t)in->tm_year + 1900;
    year += FloorDiv(month, 12);
    month -= FloorDiv(month, 12) * 12;
    const int64_t days = DaysFromCivil(year, month + 1, 1) + (int64_t)in->tm_mday - 1;
    int64_t s;
    if (__builtin_mul_overflow(days, (int64_t)86400, &s)) return false;
    const int64_t clock = (int64_t)in->tm_hour * 3600 + (int64_t)in->tm_min * 60 + in->tm_sec;
    return !__builtin_add_overflow(s, clock, &out);
}

// POSIX TZ rules ("EST5EDT,M3.2.0,M11.1.0"): a standard offset and name, and perhaps daylight time
// with the two days it starts and ends. Offsets here are seconds east of UTC, as tm_gmtoff.
struct Change {
    char kind = 'M';  // 'J': day 1-365 without Feb 29; 'D': day 0-365; 'M': month.week.weekday
    int day = 0, week = 0, month = 0;
    long secs = 7200;  // local time of the change, default 02:00
};
struct Rule {
    bool valid = false, dst = false;
    long stdOffset = 0, dstOffset = 0;
    char stdName[64] = "UTC", dstName[64] = "";
    Change start, end;
};
// A zone name: three or more letters, or <...> of letters, digits, '+' and '-'.
bool ParseName(const char*& p, char* out) {
    const char* q = p;
    size_t n = 0;
    if (*q == '<') {
        ++q;
        while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '+' || *q == '-') {
            if (n < 63) out[n++] = *q;
            ++q;
        }
        if (*q != '>') return false;
        ++q;
    } else {
        while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')) {
            if (n < 63) out[n++] = *q;
            ++q;
        }
    }
    if (n < 3) return false;
    out[n] = 0;
    p = q;
    return true;
}
// [+-]hh[:mm[:ss]] in seconds. An offset's parts are capped as glibc caps them (24 hours, 59
// minutes and seconds); a change time's are not (RFC 8536 allows -167 to 167 hours).
bool ParseTime(const char*& p, long& out, bool isOffset) {
    long sign = 1;
    if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
    if (*p < '0' || *p > '9') return false;
    long parts[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (i && (*p != ':' || p[1] < '0' || p[1] > '9')) break;
        if (i) ++p;
        while (*p >= '0' && *p <= '9') parts[i] = parts[i] * 10 + (*p++ - '0');
    }
    if (isOffset) {
        if (parts[0] > 24) parts[0] = 24;
        if (parts[1] > 59) parts[1] = 59;
        if (parts[2] > 59) parts[2] = 59;
    }
    out = sign * (parts[0] * 3600 + parts[1] * 60 + parts[2]);
    return true;
}
bool ParseChange(const char*& p, Change& c) {
    auto Number = [&](int& out) {
        if (*p < '0' || *p > '9') return false;
        out = 0;
        while (*p >= '0' && *p <= '9') out = out * 10 + (*p++ - '0');
        return true;
    };
    if (*p == 'J') {
        ++p;
        c.kind = 'J';
        if (!Number(c.day) || c.day < 1 || c.day > 365) return false;
    } else if (*p == 'M') {
        ++p;
        c.kind = 'M';
        if (!Number(c.month) || *p++ != '.' || !Number(c.week) || *p++ != '.' || !Number(c.day)) return false;
        if (c.month < 1 || c.month > 12 || c.week < 1 || c.week > 5 || c.day > 6) return false;
    } else {
        c.kind = 'D';
        if (!Number(c.day) || c.day > 365) return false;
    }
    c.secs = 7200;
    if (*p == '/') {
        ++p;
        if (!ParseTime(p, c.secs, false)) return false;
    }
    return true;
}
// As glibc reads a TZ string, including what it makes of one it can't fully read: a name it
// reads stays (with offset 0 when the offset doesn't follow), and anything else is UTC with an
// empty name. A daylight name without rules takes the United States' rules, glibc's default.
// (glibc would first look for a "posixrules" zone file for those; cordinal doesn't.)
void ParseRule(const char* p, Rule& r) {
    r = Rule{};
    r.valid = true;
    r.stdName[0] = 0;
    long west;
    if (!ParseName(p, r.stdName)) {
        r.stdName[0] = 0;
        return;
    }
    if (!ParseTime(p, west, true)) return;
    r.stdOffset = -west;
    if (!*p || !ParseName(p, r.dstName)) return;
    r.dstOffset = r.stdOffset + 3600;
    if (*p && *p != ',' && ParseTime(p, west, true)) r.dstOffset = -west;
    Change start{'M', 0, 2, 3, 7200}, end{'M', 0, 1, 11, 7200};
    if (*p == ',') {
        ++p;
        if (!ParseChange(p, start) || *p++ != ',' || !ParseChange(p, end)) return;
    } else if (*p) {
        return;
    }
    r.start = start;
    r.end = end;
    r.dst = true;
}
// When a change happens in a given year, in UTC: its local time read with `offset`. As glibc, a
// year up to 1970 counts its days from January 1, 1970 (glibc's compute_change starts every such
// year at time 0), so rule-based zones show no daylight time before 1970 in the north and only
// daylight time in the south.
int64_t ChangeTime(const Change& c, int64_t year, long offset) {
    int64_t day;
    if (c.kind == 'J') {
        day = c.day - 1 + (Leap(year) && c.day >= 60);
    } else if (c.kind == 'D') {
        day = c.day;
    } else {
        // The d'th weekday of week w (5: the last) of month m.
        const int64_t first = DaysFromCivil(year, c.month, 1);
        const int firstWeekday = (int)(((first % 7) + 11) % 7);
        int64_t mday = 1 + (c.day - firstWeekday + 7) % 7 + (int64_t)(c.week - 1) * 7;
        static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        const int length = kDays[c.month - 1] + (c.month == 2 && Leap(year));
        while (mday > length) mday -= 7;
        day = first - DaysFromCivil(year, 1, 1) + mday - 1;
    }
    const int64_t yearStart = year > 1970 ? DaysFromCivil(year, 1, 1) : 0;
    return (yearStart + day) * 86400 + c.secs - offset;
}
// The rule's state at time t, as glibc decides it: from the changes of t's year in UTC.
void RuleAt(const Rule& r, int64_t t, long& offset, int& isdst, const char*& name) {
    offset = r.stdOffset;
    isdst = 0;
    name = r.stdName;
    if (!r.dst) return;
    int64_t year;
    int m, d;
    CivilFromDays(FloorDiv(t, 86400), year, m, d);
    const int64_t start = ChangeTime(r.start, year, r.stdOffset), end = ChangeTime(r.end, year, r.dstOffset);
    const bool inDst = start > end ? (t < end || t >= start) : (t >= start && t < end);
    if (inDst) {
        offset = r.dstOffset;
        isdst = 1;
        name = r.dstName;
    }
}

// A zone: TZif transitions and types, then perhaps a rule for times after the last transition.
// Zones are never freed, so tm_zone pointers into them stay valid after TZ changes.
struct Type {
    long offset;
    int isdst;
    const char* name;
};
struct Zone {
    int64_t* times = nullptr;
    uint8_t* types = nullptr;
    size_t count = 0;
    Type kinds[256];
    size_t kindCount = 0;
    char* names = nullptr;
    Rule rule;  // the TZif footer, or the whole zone for a TZ string
    bool fromFile = false;
    bool tzSet = false;     // TZ was set (perhaps to "")
    char source[256] = "";  // TZ's value
    dev_t device = 0;
    ino_t inode = 0;
    int64_t modified = 0;
};

uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
int64_t Be64(const uint8_t* p) { return (int64_t)((uint64_t)Be32(p) << 32 | Be32(p + 4)); }

bool ReadAll(const char* path, uint8_t*& data, size_t& size, struct stat& info) {
    const int fd = (int)cordlibc::Syscall(__NR_openat, -100, (long)path, 0x80000 /* O_RDONLY | O_CLOEXEC */);
    if (fd < 0) return false;
    bool ok = cordlibc::Syscall(__NR_fstat, fd, (long)&info) == 0 && info.st_size > 44 && info.st_size < (1 << 22);
    if (ok) {
        size = (size_t)info.st_size;
        data = (uint8_t*)malloc(size);
        size_t got = 0;
        while (data && got < size) {
            const long n = cordlibc::Syscall(__NR_read, fd, (long)(data + got), (long)(size - got));
            if (n <= 0) break;
            got += (size_t)n;
        }
        ok = data && got == size;
        if (!ok) free(data);
    }
    cordlibc::Syscall(__NR_close, fd);
    return ok;
}
// A TZif file (version 1, or the 64-bit data and footer of versions 2 and later).
bool ParseTzif(const uint8_t* data, size_t size, Zone& z) {
    if (size < 44 || memcmp(data, "TZif", 4)) return false;
    const uint8_t version = data[4];
    const uint8_t* p = data;
    auto Counts = [&](const uint8_t* h, uint32_t c[6]) {
        for (int i = 0; i < 6; ++i) c[i] = Be32(h + 20 + 4 * i);
    };
    uint32_t c[6];  // isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt
    Counts(p, c);
    size_t timeSize = 4;
    if (version >= '2') {
        // Skip the 32-bit block to the second header.
        const size_t v1 = (size_t)c[3] * 5 + (size_t)c[4] * 6 + c[5] + (size_t)c[2] * 8 + c[1] + c[0];
        if (44 + v1 + 44 > size) return false;
        p = data + 44 + v1;
        if (memcmp(p, "TZif", 4)) return false;
        Counts(p, c);
        timeSize = 8;
    }
    const uint32_t timecnt = c[3], typecnt = c[4], charcnt = c[5];
    if (!typecnt || typecnt > 256) return false;
    const size_t body = (size_t)timecnt * (timeSize + 1) + (size_t)typecnt * 6 + charcnt +
                        (size_t)c[2] * (timeSize + 4) + c[1] + c[0];
    const uint8_t* b = p + 44;
    if ((size_t)(b - data) + body > size) return false;
    z.count = timecnt;
    z.times = (int64_t*)malloc(sizeof(int64_t) * (timecnt ? timecnt : 1));
    z.types = (uint8_t*)malloc(timecnt ? timecnt : 1);
    z.names = (char*)malloc(charcnt + 1);
    if (!z.times || !z.types || !z.names) return false;
    for (uint32_t i = 0; i < timecnt; ++i) z.times[i] = timeSize == 8 ? Be64(b + 8 * i) : (int32_t)Be32(b + 4 * i);
    b += (size_t)timecnt * timeSize;
    for (uint32_t i = 0; i < timecnt; ++i) {
        if (b[i] >= typecnt) return false;
        z.types[i] = b[i];
    }
    b += timecnt;
    const uint8_t* typeData = b;
    b += (size_t)typecnt * 6;
    memcpy(z.names, b, charcnt);
    z.names[charcnt] = 0;
    for (uint32_t i = 0; i < typecnt; ++i) {
        const uint8_t* t = typeData + 6 * i;
        z.kinds[i].offset = (int32_t)Be32(t);
        z.kinds[i].isdst = t[4] ? 1 : 0;
        z.kinds[i].name = t[5] < charcnt ? z.names + t[5] : "";
    }
    z.kindCount = typecnt;
    b += charcnt + (size_t)c[2] * (timeSize + 4) + c[1] + c[0];
    // The footer: "\n<TZ string>\n", used for times after the last transition.
    if (timeSize == 8 && b < data + size && *b == '\n') {
        const uint8_t* end = (const uint8_t*)memchr(b + 1, '\n', (size_t)(data + size - b - 1));
        if (end && end - b - 1 < 128) {
            char text[128];
            memcpy(text, b + 1, (size_t)(end - b - 1));
            text[end - b - 1] = 0;
            if (*text) ParseRule(text, z.rule);
        }
    }
    z.fromFile = true;
    return true;
}
bool LoadFile(const char* path, Zone& z) {
    uint8_t* data = nullptr;
    size_t size = 0;
    struct stat info;
    if (!ReadAll(path, data, size, info)) return false;
    const bool ok = ParseTzif(data, size, z);
    free(data);
    if (ok) {
        z.device = info.st_dev;
        z.inode = info.st_ino;
        z.modified = (int64_t)info.st_mtim.tv_sec * 1000000000 + info.st_mtim.tv_nsec;
    }
    return ok;
}

// The zone in use, read the way glibc reads TZ: unset means /etc/localtime (UTC without it);
// empty means "Universal"; a leading ':' is dropped; a value names a file (absolute, or under
// TZDIR or /usr/share/zoneinfo) and otherwise is a POSIX rule (see ParseRule).
pthread_mutex_t zoneLock = PTHREAD_MUTEX_INITIALIZER;
Zone* zone = nullptr;
Zone* Load() {
    const char* tz = getenv("TZ");
    Zone* z = (Zone*)calloc(1, sizeof(Zone));
    if (!z) return nullptr;
    new (z) Zone();
    if (tz) {
        z->tzSet = true;
        size_t n = strlen(tz);
        if (n >= sizeof z->source) n = sizeof z->source - 1;
        memcpy(z->source, tz, n);
    }
    if (!tz) {
        LoadFile("/etc/localtime", *z);
    } else {
        const char* name = !*tz ? "Universal" : *tz == ':' ? tz + 1 : tz;  // glibc's reading of ""
        char path[512];
        bool loaded = false;
        if (*name == '/') {
            loaded = LoadFile(name, *z);
        } else if (*name && !strstr(name, "..")) {
            const char* dir = getenv("TZDIR");
            if (!dir || !*dir) dir = "/usr/share/zoneinfo";
            const size_t dn = strlen(dir), nn = strlen(name);
            if (dn + nn + 2 < sizeof path) {
                memcpy(path, dir, dn);
                path[dn] = '/';
                memcpy(path + dn + 1, name, nn + 1);
                loaded = LoadFile(path, *z);
            }
        }
        if (!loaded) ParseRule(name, z->rule);
    }
    if (!z->fromFile && !z->rule.valid) z->rule = Rule{true, false, 0, 0, "UTC", "", {}, {}};
    return z;
}
// Whether the zone in use still matches TZ (and, for a file, the file on disk).
bool Current(const Zone* z) {
    const char* tz = getenv("TZ");
    if ((tz != nullptr) != z->tzSet) return false;
    if (tz && strncmp(tz, z->source, sizeof z->source - 1)) return false;
    if (!tz && z->fromFile) {
        struct stat info;
        if (cordlibc::Syscall(__NR_newfstatat, -100, (long)"/etc/localtime", (long)&info, 0)) return !z->device;
        const int64_t modified = (int64_t)info.st_mtim.tv_sec * 1000000000 + info.st_mtim.tv_nsec;
        return info.st_dev == z->device && info.st_ino == z->inode && modified == z->modified;
    }
    return true;
}
// The zone, loaded on first use; `recheck` (tzset, mktime) reloads it if TZ or the file changed,
// as glibc's tzset does. localtime_r checks only the first time, as glibc's does.
const Zone* Use(bool recheck) {
    pthread_mutex_lock(&zoneLock);
    if (!zone || (recheck && !Current(zone))) {
        if (Zone* fresh = Load()) zone = fresh;
    }
    const Zone* z = zone;
    pthread_mutex_unlock(&zoneLock);
    return z;
}
// The zone's offset, daylight flag and name at time t.
void ZoneAt(const Zone* z, int64_t t, long& offset, int& isdst, const char*& name) {
    if (!z->fromFile) {
        RuleAt(z->rule, t, offset, isdst, name);
        return;
    }
    const Type* kind;
    if (!z->count || t < z->times[0]) {
        // Before the first transition: glibc takes the first type that isn't daylight time.
        size_t i = 0;
        while (i < z->kindCount && z->kinds[i].isdst) ++i;
        kind = &z->kinds[i < z->kindCount ? i : 0];
        if (z->count == 0 && z->rule.valid) {
            RuleAt(z->rule, t, offset, isdst, name);
            return;
        }
    } else if (t >= z->times[z->count - 1]) {
        if (z->rule.valid) {
            RuleAt(z->rule, t, offset, isdst, name);
            return;
        }
        kind = &z->kinds[z->types[z->count - 1]];
    } else {
        size_t lo = 0, hi = z->count - 1;  // times[lo] <= t < times[hi]
        while (hi - lo > 1) {
            const size_t mid = (lo + hi) / 2;
            if (t >= z->times[mid]) lo = mid;
            else hi = mid;
        }
        kind = &z->kinds[z->types[lo]];
    }
    offset = kind->offset;
    isdst = kind->isdst;
    name = kind->name;
}
}  // namespace

// For the checks: which vDSO functions were found (1 clock_gettime, 2 gettimeofday, 4 time).
extern "C" __attribute__((visibility("hidden"))) int cordinal_time_vdso(void) {
    return (vdsoClock ? 1 : 0) | (vdsoTimeOfDay ? 2 : 0) | (vdsoTime ? 4 : 0);
}

// For strftime: the current zone's name for a daylight flag, which is what glibc's tzname[] holds
// when a tm has no tm_zone of its own. A zone loaded from a file keeps the names its types carry, so
// the first type with the flag asked for answers; a rule-based zone names its two sides.
extern "C" __attribute__((visibility("hidden"))) const char* cordinal_zone_name(int isdst) {
    const Zone* z = Use(false);
    if (!z) return "UTC";
    if (!z->fromFile) return isdst ? z->rule.dstName : z->rule.stdName;
    for (size_t i = 0; i < z->kindCount; ++i)
        if (z->kinds[i].isdst == (isdst != 0)) return z->kinds[i].name;
    return z->kindCount ? z->kinds[0].name : "UTC";
}

#define HIDE(name) __asm__(".hidden " #name);
HIDE(clock_gettime) HIDE(gettimeofday) HIDE(time) HIDE(difftime) HIDE(gmtime_r) HIDE(timegm) HIDE(localtime_r)
HIDE(mktime) HIDE(tzset)
#undef HIDE

extern "C" {
int clock_gettime(clockid_t clock, struct timespec* ts) noexcept {
    if (vdsoClock) return (int)cordlibc::Posix(vdsoClock(clock, ts));
    return (int)cordlibc::Call(__NR_clock_gettime, clock, ts);
}
int gettimeofday(struct timeval* __restrict tv, void* __restrict tz) noexcept {
    if (vdsoTimeOfDay) return (int)cordlibc::Posix(vdsoTimeOfDay(tv, tz));
    return (int)cordlibc::Call(__NR_gettimeofday, tv, tz);
}
time_t time(time_t* out) noexcept {
    time_t now;
    if (vdsoTime) {
        now = vdsoTime(nullptr);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        now = ts.tv_sec;
    }
    if (out) *out = now;
    return now;
}
double difftime(time_t a, time_t b) noexcept { return (double)((__int128)a - b); }

struct tm* gmtime_r(const time_t* t, struct tm* out) noexcept {
    if (!BreakDown(*t, 0, 0, "GMT", out)) {
        errno = EOVERFLOW;
        return nullptr;
    }
    return out;
}
time_t timegm(struct tm* in) noexcept {
    int64_t t;
    if (!LocalSeconds(in, t) || !BreakDown(t, 0, 0, "GMT", in)) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    return t;
}
void tzset(void) noexcept { Use(true); }
struct tm* localtime_r(const time_t* t, struct tm* out) noexcept {
    const Zone* z = Use(false);
    long offset = 0;
    int isdst = 0;
    const char* name = "UTC";
    if (z) ZoneAt(z, *t, offset, isdst, name);
    if (!BreakDown(*t, offset, isdst, name, out)) {
        errno = EOVERFLOW;
        return nullptr;
    }
    return out;
}
// The time whose local reading is the tm's fields (carried as needed), searched for as glibc
// (gnulib's mktime) searches, so the same calls give the same answers:
// - Seconds outside 0-59 are set aside, and added back once the offset is found.
// - The search starts from the offset the previous call found, then follows the offset of each
//   guess, up to six times. Where the clock repeats an hour, that start decides which reading
//   is found.
// - Where the clock skips ahead, the guesses bounce between the two sides; glibc takes the side
//   whose daylight flag differs from the one asked for (see the loop), or fails.
// - Where the found time's tm_isdst disagrees with the one asked for, use the offset of the
//   nearest time that has it, probing a week (601,200 s) at a time both ways for about seven and
//   a quarter years; with none, assume the two differ by an hour.
static long lastOffset = 0;  // glibc's localtime_offset; racy across threads in glibc too
time_t mktime(struct tm* in) noexcept {
    const Zone* z = Use(true);
    tm fields = *in;
    const int requestedSeconds = fields.tm_sec;
    fields.tm_sec = requestedSeconds < 0 ? 0 : requestedSeconds > 59 ? 59 : requestedSeconds;
    int64_t local;
    if (!z || !LocalSeconds(&fields, local)) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    const long guess = __atomic_load_n(&lastOffset, __ATOMIC_RELAXED);
    long offset;
    int isdst;
    const char* name;
    // glibc's loop: t, t1 and t2 are this guess and the two before it; dst2 is the previous
    // guess's daylight flag. A gap shows as t coming back to t1. glibc takes the guess there if
    // its flag differs from the one asked for or, for -1, if it is daylight time or the previous
    // guess wasn't; otherwise it gives up after six guesses (EOVERFLOW), leaving the tm alone.
    int64_t t = local - guess, t1 = t, t2 = t;
    bool dst2 = false, gap = false;
    for (int remaining = 6;; t1 = t2, t2 = t, t = local - offset, dst2 = isdst != 0) {
        ZoneAt(z, t, offset, isdst, name);
        if (local - offset == t) break;
        const bool now = isdst != 0;
        if (t == t1 && t != t2 && (in->tm_isdst < 0 ? dst2 <= now : (in->tm_isdst != 0) != now)) {
            gap = true;
            break;
        }
        if (--remaining == 0) {
            errno = EOVERFLOW;
            return (time_t)-1;
        }
    }
    ZoneAt(z, t, offset, isdst, name);
    if (!gap && in->tm_isdst >= 0 && isdst != (in->tm_isdst > 0)) {
        constexpr int64_t kStride = 601200, kBound = 457243209 / 2 + kStride;
        bool matched = false;
        for (int64_t delta = kStride; delta < kBound && !matched; delta += kStride) {
            for (int64_t sign = -1; sign <= 1 && !matched; sign += 2) {
                long o;
                int d;
                const char* n;
                ZoneAt(z, t + sign * delta, o, d, n);
                if (d == (in->tm_isdst > 0)) {
                    t = local - o;
                    matched = true;
                }
            }
        }
        if (!matched) t += 3600 * ((in->tm_isdst == 0) - (isdst == 0));
    }
    __atomic_store_n(&lastOffset, (long)(local - t), __ATOMIC_RELAXED);
    t += requestedSeconds - fields.tm_sec;
    ZoneAt(z, t, offset, isdst, name);
    if (!BreakDown(t, offset, isdst, name, in)) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    return t;
}
}  // extern "C"
