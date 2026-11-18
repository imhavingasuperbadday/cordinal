// cordinal's own C library, the last imports after stages 1 to 7: strerror and strerror_r, sysconf
// and get_nprocs, getpwuid, nice, shmctl, signal, fnmatch, qsort_r, wcscoll_l, the gettext calls and
// arc4random, and strtof128 and strfromf128 after them. glibc's names with hidden visibility like the
// rest of the layer, so cordinal and the static libstdc++ bind to these while the program exports
// nothing. No glibc header that declares these functions is included: the definitions are the
// declarations, and the structures passed through (passwd, shmid_ds) are declared here with glibc's
// layout, which misc_checks.h checks field by field against the system's headers. The strings
// strerror serves are the C locale's English, the layer's one locale: glibc translates them through
// gettext, and this library has nothing to translate them with.
#include "syscall.h"
#include <initializer_list>
#include <stddef.h>
#include <stdint.h>

extern "C" void* malloc(size_t);
extern "C" void free(void*);
extern "C" __attribute__((noreturn)) void abort(void) noexcept;

// -------------------------------------------------------------------------------------------------
// Reading the system's files: every answer sysconf, getpwuid, get_nprocs and the signal stack size
// give comes from one of them, and a missing or unreadable file is the caller's fallback rather than
// an error of the call being served.

namespace cordlibc {
namespace {

constexpr long kAtFdCwd = -100;
constexpr long kReadOnly = 0;

// The kernel's own resource numbers (asm/resource.h) and struct rlimit.
constexpr int kRlimitStack = 3;
constexpr int kRlimitNoFile = 7;
constexpr int kRlimitNproc = 6;
constexpr int kRlimitSigPending = 11;

struct KernelRlimit {
    unsigned long current, maximum;
};

bool ReadLimit(int resource, unsigned long& value) {
    KernelRlimit limit{};
    const int saved = errno;
    const bool ok = cordlibc::Call(__NR_prlimit64, 0, resource, nullptr, &limit) >= 0;
    errno = saved;  // asking the system a question is not the caller's error
    if (!ok) return false;
    value = limit.current;
    return true;
}

// A whole file into `buffer`, NUL-terminated when there is room. A missing or unreadable file is the
// caller's fallback, never a failure of the call being served, so errno is left as it was.
int ReadWholeFile(const char* path, char* buffer, int size) {
    const int saved = errno;
    const int fd = (int)cordlibc::Call(__NR_openat, kAtFdCwd, path, kReadOnly, 0);
    if (fd < 0) {
        errno = saved;
        return -1;
    }
    int got = 0;
    while (got < size - 1) {
        const long n = cordlibc::Call(__NR_read, fd, buffer + got, (unsigned)(size - 1 - got));
        if (n <= 0) break;
        got += (int)n;
    }
    cordlibc::Call(__NR_close, fd);
    buffer[got] = 0;
    errno = saved;
    return got;
}

int ParseInt(const char* text, int fallback) {
    int value = 0;
    bool any = false;
    for (; *text >= '0' && *text <= '9'; ++text) {
        value = value * 10 + (*text - '0');
        any = true;
    }
    return any ? value : fallback;
}

// One number out of a text like /proc/meminfo's "MemTotal: 16309916 kB".
bool ReadNumberAfter(const char* text, const char* key, int& out) {
    const int length = (int)__builtin_strlen(key);
    for (const char* at = text; *at; ++at) {
        if (at != text && at[-1] != '\n') continue;
        if (__builtin_strncmp(at, key, (size_t)length) != 0) continue;
        const char* number = at + length;
        while (*number == ' ' || *number == '\t') ++number;
        out = ParseInt(number, -1);
        return out >= 0;
    }
    return false;
}

// A value from the auxiliary vector (/proc/self/auxv), or `fallback` when the kernel has no such
// entry. The records are the kernel's own: an 8-byte type and an 8-byte value, ended by a zero type.
long Auxval(long type, long fallback) {
    char text[512];
    const int got = ReadWholeFile("/proc/self/auxv", text, sizeof text);
    for (int at = 0; got > 0 && at + 16 <= got; at += 16) {
        long entry[2] = {0, 0};
        for (int i = 0; i < 2; ++i) {
            long value = 0;
            for (int b = 7; b >= 0; --b) value = (value << 8) | (unsigned char)text[at + i * 8 + b];
            entry[i] = value;
        }
        if (entry[0] == 0) break;
        if (entry[0] == type) return entry[1];
    }
    return fallback;
}


}  // namespace
}  // namespace cordlibc

#define CL extern "C" __attribute__((visibility("hidden")))

using namespace cordlibc;
using cordlibc::ParseInt;
using cordlibc::ReadWholeFile;

// -------------------------------------------------------------------------------------------------
// strerror and strerror_r: the C locale's English for every errno x86-64 Linux has, 0 to 134 (41 and
// 58 have no message, as in glibc's table). glibc's own strings are gettext's translations of these,
// and the program stays in the C locale, so this is what glibc answers here as well.

namespace {

const char* const kErrors[] = {
    /* 0 */ "Success",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted system call",
    "Input/output error",
    "No such device or address",
    "Argument list too long",
    "Exec format error",
    "Bad file descriptor",
    /* 10 */ "No child processes",
    "Resource temporarily unavailable",
    "Cannot allocate memory",
    "Permission denied",
    "Bad address",
    "Block device required",
    "Device or resource busy",
    "File exists",
    "Invalid cross-device link",
    "No such device",
    /* 20 */ "Not a directory",
    "Is a directory",
    "Invalid argument",
    "Too many open files in system",
    "Too many open files",
    "Inappropriate ioctl for device",
    "Text file busy",
    "File too large",
    "No space left on device",
    "Illegal seek",
    /* 30 */ "Read-only file system",
    "Too many links",
    "Broken pipe",
    "Numerical argument out of domain",
    "Numerical result out of range",
    "Resource deadlock avoided",
    "File name too long",
    "No locks available",
    "Function not implemented",
    "Directory not empty",
    /* 40 */ "Too many levels of symbolic links",
    nullptr,  // 41
    "No message of desired type",
    "Identifier removed",
    "Channel number out of range",
    "Level 2 not synchronized",
    "Level 3 halted",
    "Level 3 reset",
    "Link number out of range",
    "Protocol driver not attached",
    /* 50 */ "No CSI structure available",
    "Level 2 halted",
    "Invalid exchange",
    "Invalid request descriptor",
    "Exchange full",
    "No anode",
    "Invalid request code",
    "Invalid slot",
    nullptr,  // 58
    "Bad font file format",
    /* 60 */ "Device not a stream",
    "No data available",
    "Timer expired",
    "Out of streams resources",
    "Machine is not on the network",
    "Package not installed",
    "Object is remote",
    "Link has been severed",
    "Advertise error",
    "Srmount error",
    /* 70 */ "Communication error on send",
    "Protocol error",
    "Multihop attempted",
    "RFS specific error",
    "Bad message",
    "Value too large for defined data type",
    "Name not unique on network",
    "File descriptor in bad state",
    "Remote address changed",
    "Can not access a needed shared library",
    /* 80 */ "Accessing a corrupted shared library",
    ".lib section in a.out corrupted",
    "Attempting to link in too many shared libraries",
    "Cannot exec a shared library directly",
    "Invalid or incomplete multibyte or wide character",
    "Interrupted system call should be restarted",
    "Streams pipe error",
    "Too many users",
    "Socket operation on non-socket",
    "Destination address required",
    /* 90 */ "Message too long",
    "Protocol wrong type for socket",
    "Protocol not available",
    "Protocol not supported",
    "Socket type not supported",
    "Operation not supported",
    "Protocol family not supported",
    "Address family not supported by protocol",
    "Address already in use",
    "Cannot assign requested address",
    /* 100 */ "Network is down",
    "Network is unreachable",
    "Network dropped connection on reset",
    "Software caused connection abort",
    "Connection reset by peer",
    "No buffer space available",
    "Transport endpoint is already connected",
    "Transport endpoint is not connected",
    "Cannot send after transport endpoint shutdown",
    "Too many references: cannot splice",
    /* 110 */ "Connection timed out",
    "Connection refused",
    "Host is down",
    "No route to host",
    "Operation already in progress",
    "Operation now in progress",
    "Stale file handle",
    "Structure needs cleaning",
    "Not a XENIX named type file",
    "No XENIX semaphores available",
    /* 120 */ "Is a named type file",
    "Remote I/O error",
    "Disk quota exceeded",
    "No medium found",
    "Wrong medium type",
    "Operation canceled",
    "Required key not available",
    "Key has expired",
    "Key has been revoked",
    "Key was rejected by service",
    /* 130 */ "Owner died",
    "State not recoverable",
    "Operation not possible due to RF-kill",
    "Memory page has hardware error",
    "Inappropriate file type or format",
};

constexpr int kErrorCount = (int)(sizeof kErrors / sizeof *kErrors);

// glibc's "Unknown error N", cut into `out` the way it cuts it: nothing at all for a size of zero,
// otherwise the first size - 1 bytes and a terminator. Returns the whole length.
int FormatUnknown(int errnum, char* out, size_t size) {
    char text[24];
    int length = 0;
    for (const char* prefix = "Unknown error "; *prefix; ++prefix) text[length++] = *prefix;
    unsigned value = errnum < 0 ? 0u - (unsigned)errnum : (unsigned)errnum;
    if (errnum < 0) text[length++] = '-';
    char digits[10];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) text[length++] = digits[--count];
    if (size) {
        const size_t copied = (size_t)length < size - 1 ? (size_t)length : size - 1;
        for (size_t i = 0; i < copied; ++i) out[i] = text[i];
        out[copied] = 0;
    }
    return length;
}

// strerror's buffer for a number it has no message for. glibc keeps one per thread; cordinal has no
// thread-local storage before stage 8, so this is one shared buffer, like the layer's other static
// results. Nothing in the program calls strerror from two threads at once.
char gUnknown[32];

}  // namespace

CL char* strerror(int errnum) {
    if (errnum >= 0 && errnum < kErrorCount && kErrors[errnum]) return const_cast<char*>(kErrors[errnum]);
    FormatUnknown(errnum, gUnknown, sizeof gUnknown);
    return gUnknown;
}

// The GNU form, which is the one _GNU_SOURCE's header declares: a number with a message comes back as
// glibc's own static string whatever the buffer's size, and only an unknown number is written into
// the caller's buffer. errno is never touched, as glibc's leaves it.
CL char* strerror_r(int errnum, char* buffer, size_t size) {
    if (errnum >= 0 && errnum < kErrorCount && kErrors[errnum]) return const_cast<char*>(kErrors[errnum]);
    FormatUnknown(errnum, buffer, size);
    return buffer;
}

// -------------------------------------------------------------------------------------------------
// sysconf: the answers glibc gives on this kernel for the names it answers. Its numbers are glibc's
// own enum (a caller's constant comes from its header), so they are written out here and misc_checks.h
// holds every one against unistd.h. A name glibc has no answer for (an unknown one included) is -1
// with EINVAL; a name glibc answers "no limit" for is -1 and leaves errno alone.

namespace {

enum Sc : int {
    ScArgMax = 0, ScChildMax, ScClkTck, ScNgroupsMax, ScOpenMax, ScStreamMax, ScTznameMax,
    ScJobControl, ScSavedIds, ScRealtimeSignals, ScPriorityScheduling, ScTimers, ScAsynchronousIo,
    ScPrioritizedIo, ScSynchronizedIo, ScFsync, ScMappedFiles, ScMemlock, ScMemlockRange,
    ScMemoryProtection, ScMessagePassing, ScSemaphores, ScSharedMemoryObjects, ScAioListioMax,
    ScAioMax, ScAioPrioDeltaMax, ScDelaytimerMax, ScMqOpenMax, ScMqPrioMax, ScVersion, ScPagesize,
    ScRtsigMax, ScSemNsemsMax, ScSemValueMax, ScSigqueueMax, ScTimerMax, ScBcBaseMax, ScBcDimMax,
    ScBcScaleMax, ScBcStringMax, ScCollWeightsMax, ScEquivClassMax, ScExprNestMax, ScLineMax,
    ScReDupMax, ScCharclassNameMax, Sc2Version, Sc2CBind, Sc2CDev, Sc2FortDev, Sc2FortRun, Sc2SwDev,
    Sc2Localedef, ScPii, ScPiiXti, ScPiiSocket, ScPiiInternet, ScPiiOsi, ScPoll, ScSelect, ScIovMax,
    ScPiiInternetStream, ScPiiInternetDgram, ScPiiOsiCots, ScPiiOsiClts, ScPiiOsiM, ScTIovMax,
    ScThreads, ScThreadSafeFunctions, ScGetgrRSizeMax, ScGetpwRSizeMax, ScLoginNameMax,
    ScTtyNameMax, ScThreadDestructorIterations, ScThreadKeysMax, ScThreadStackMin,
    ScThreadThreadsMax, ScThreadAttrStackaddr, ScThreadAttrStacksize, ScThreadPriorityScheduling,
    ScThreadPrioInherit, ScThreadPrioProtect, ScThreadProcessShared, ScNprocessorsConf,
    ScNprocessorsOnln, ScPhysPages, ScAvphysPages, ScAtexitMax, ScPassMax, ScXopenVersion,
    ScXopenXcuVersion, ScXopenUnix, ScXopenCrypt, ScXopenEnhI18n, ScXopenShm, Sc2CharTerm,
    Sc2CVersion, Sc2Upe, ScXopenXpg2, ScXopenXpg3, ScXopenXpg4, ScCharBit, ScCharMax, ScCharMin,
    ScIntMax, ScIntMin, ScLongBit, ScWordBit, ScMbLenMax, ScNzero, ScSsizeMax, ScScharMax,
    ScScharMin, ScShrtMax, ScShrtMin, ScUcharMax, ScUintMax, ScUlongMax, ScUshrtMax, ScNlArgmax,
    ScNlLangmax, ScNlMsgmax, ScNlNmax, ScNlSetmax, ScNlTextmax, ScXbs5Ilp32Off32,
    ScXbs5Ilp32Offbig, ScXbs5Lp64Off64, ScXbs5LpbigOffbig, ScXopenLegacy, ScXopenRealtime,
    ScXopenRealtimeThreads, ScAdvisoryInfo, ScBarriers, ScBase, ScCLangSupport, ScCLangSupportR,
    ScClockSelection, ScCputime, ScThreadCputime, ScDeviceIo, ScDeviceSpecific, ScDeviceSpecificR,
    ScFdMgmt, ScFifo, ScPipe, ScFileAttributes, ScFileLocking, ScFileSystem, ScMonotonicClock,
    ScMultiProcess, ScSingleProcess, ScNetworking, ScReaderWriterLocks, ScSpinLocks, ScRegexp,
    ScRegexVersion, ScShell, ScSignals, ScSpawn, ScSporadicServer, ScThreadSporadicServer,
    ScSystemDatabase, ScSystemDatabaseR, ScTimeouts, ScTypedMemoryObjects, ScUserGroups,
    ScUserGroupsR, Sc2Pbs, Sc2PbsAccounting, Sc2PbsLocate, Sc2PbsMessage, Sc2PbsTrack, ScSymloopMax,
    ScStreams, Sc2PbsCheckpoint, ScV6Ilp32Off32, ScV6Ilp32Offbig, ScV6Lp64Off64, ScV6LpbigOffbig,
    ScHostNameMax, ScTrace, ScTraceEventFilter, ScTraceInherit, ScTraceLog,
    // The cache names run from 185 to 199; there is a gap between the two halves of glibc's enum.
    ScLevel1IcacheSize = 185, ScLevel1IcacheAssoc, ScLevel1IcacheLinesize, ScLevel1DcacheSize,
    ScLevel1DcacheAssoc, ScLevel1DcacheLinesize, ScLevel2CacheSize, ScLevel2CacheAssoc,
    ScLevel2CacheLinesize, ScLevel3CacheSize, ScLevel3CacheAssoc, ScLevel3CacheLinesize,
    ScLevel4CacheSize, ScLevel4CacheAssoc, ScLevel4CacheLinesize,
    ScIpv6 = 235, ScRawSockets, ScV7Ilp32Off32, ScV7Ilp32Offbig, ScV7Lp64Off64, ScV7LpbigOffbig,
    ScSsReplMax, ScTraceEventNameMax, ScTraceNameMax, ScTraceSysMax, ScTraceUserEventMax,
    ScXopenStreams, ScThreadRobustPrioInherit, ScThreadRobustPrioProtect, ScMinsigstksz, ScSigstksz,
};

constexpr long kPosix200809 = 200809;

// The names glibc answers from constants rather than from the running system; -2 means "not one of
// those", which the caller then decides from the system or refuses.
long ConstantSc(int name) {
    switch (name) {
        case ScClkTck: return 100;
        case ScStreamMax: return 16;
        case ScJobControl:
        case ScSavedIds: return 1;
        case ScRealtimeSignals:
        case ScPriorityScheduling:
        case ScTimers:
        case ScAsynchronousIo:
        case ScPrioritizedIo:
        case ScSynchronizedIo:
        case ScFsync:
        case ScMappedFiles:
        case ScMemlock:
        case ScMemlockRange:
        case ScMemoryProtection:
        case ScMessagePassing:
        case ScSemaphores:
        case ScSharedMemoryObjects:
        case ScVersion:
        case Sc2Version:
        case Sc2CBind:
        case Sc2CDev:
        case Sc2SwDev:
        case Sc2Localedef:
        case ScThreads:
        case ScThreadSafeFunctions:
        case ScThreadAttrStackaddr:
        case ScThreadAttrStacksize:
        case ScThreadPriorityScheduling:
        case ScThreadPrioInherit:
        case ScThreadPrioProtect:
        case ScThreadProcessShared:
        case Sc2CharTerm:
        case Sc2CVersion:
        case ScAdvisoryInfo:
        case ScBarriers:
        case ScClockSelection:
        case ScCputime:
        case ScThreadCputime:
        case ScMonotonicClock:
        case ScReaderWriterLocks:
        case ScSpinLocks:
        case ScSpawn:
        case ScTimeouts:
        case ScIpv6:
        case ScRawSockets: return kPosix200809;
        case ScAioPrioDeltaMax: return 20;
        case ScDelaytimerMax:
        case ScSemValueMax:
        case ScAtexitMax:
        case ScNlMsgmax:
        case ScNlNmax:
        case ScNlSetmax:
        case ScNlTextmax: return 2147483647L;
        case ScMqPrioMax: return 32768;
        case ScRtsigMax: return 32;
        case ScBcBaseMax: return 99;
        case ScBcDimMax: return 2048;
        case ScBcScaleMax: return 99;
        case ScBcStringMax: return 1000;
        case ScCollWeightsMax: return 255;
        case ScExprNestMax: return 32;
        case ScLineMax: return 2048;
        case ScReDupMax: return 32767;
        case ScCharclassNameMax: return 2048;
        case ScXopenVersion: return 700;
        case ScXopenXcuVersion: return 4;
        case ScXopenUnix:
        case ScXopenEnhI18n:
        case ScXopenShm:
        case ScXopenXpg2:
        case ScXopenXpg3:
        case ScXopenXpg4:
        case ScXopenLegacy:
        case ScXopenRealtime:
        case ScXopenRealtimeThreads:
        case ScRegexp:
        case ScShell:
        case ScXbs5Lp64Off64:
        case ScV6Lp64Off64:
        case ScV7Lp64Off64: return 1;
        case ScCharBit: return 8;
        case ScCharMax: return 127;
        case ScCharMin: return -128;
        case ScIntMax: return 2147483647;
        case ScIntMin: return -2147483648L;
        case ScLongBit: return 64;
        case ScWordBit: return 32;
        case ScMbLenMax: return 16;
        case ScNzero: return 20;
        case ScSsizeMax: return 32767;
        case ScScharMax: return 127;
        case ScScharMin: return -128;
        case ScShrtMax: return 32767;
        case ScShrtMin: return -32768;
        case ScUcharMax: return 255;
        case ScUintMax: return 4294967295L;
        case ScUlongMax: return -1;
        case ScUshrtMax: return 65535;
        case ScNlArgmax: return 4096;
        case ScNlLangmax: return 2048;
        case ScPassMax: return 8192;
        case ScThreadDestructorIterations: return 4;
        case ScThreadKeysMax: return 1024;
        case ScThreadStackMin: return 16384;
        case ScGetpwRSizeMax:
        case ScGetgrRSizeMax: return 1024;
        case ScLoginNameMax: return 256;
        case ScTtyNameMax: return 32;
        case ScHostNameMax: return 64;
        case ScIovMax: return 1024;
        case ScTznameMax:
        case ScAioListioMax:
        case ScAioMax:
        case ScMqOpenMax:
        case ScSemNsemsMax:
        case ScTimerMax:
        case Sc2FortDev:
        case Sc2FortRun:
        case Sc2Upe:
        case ScBase:
        case ScCLangSupport:
        case ScCLangSupportR:
        case ScDeviceIo:
        case ScDeviceSpecific:
        case ScDeviceSpecificR:
        case ScFdMgmt:
        case ScFifo:
        case ScPipe:
        case ScFileAttributes:
        case ScFileLocking:
        case ScFileSystem:
        case ScMultiProcess:
        case ScSingleProcess:
        case ScNetworking:
        case ScRegexVersion:
        case ScSignals:
        case ScSporadicServer:
        case ScThreadSporadicServer:
        case ScThreadThreadsMax:
        case ScSystemDatabase:
        case ScSystemDatabaseR:
        case ScTypedMemoryObjects:
        case ScUserGroups:
        case ScUserGroupsR:
        case Sc2Pbs:
        case Sc2PbsAccounting:
        case Sc2PbsLocate:
        case Sc2PbsMessage:
        case Sc2PbsTrack:
        case ScSymloopMax:
        case ScTrace:
        case ScTraceEventFilter:
        case ScTraceInherit:
        case ScTraceLog:
        case ScTraceNameMax:
        case ScTraceSysMax:
        case ScTraceUserEventMax:
        case ScTraceEventNameMax:
        case ScStreams:
        case Sc2PbsCheckpoint:
        case ScPii:
        case ScPiiXti:
        case ScPiiSocket:
        case ScPiiInternet:
        case ScPiiOsi:
        case ScPoll:
        case ScSelect:
        case ScPiiInternetStream:
        case ScPiiInternetDgram:
        case ScPiiOsiCots:
        case ScPiiOsiClts:
        case ScPiiOsiM:
        case ScTIovMax:
        case ScXopenCrypt:
        case ScXopenStreams:
        case ScXbs5Ilp32Off32:
        case ScXbs5Ilp32Offbig:
        case ScXbs5LpbigOffbig:
        case ScV7Ilp32Off32:
        case ScV7Ilp32Offbig:
        case ScV7LpbigOffbig:
        case ScV6Ilp32Off32:
        case ScV6Ilp32Offbig:
        case ScV6LpbigOffbig: return -1;
        case ScMinsigstksz:
        case ScSigstksz:
        case ScPagesize:
        case ScArgMax:
        case ScChildMax:
        case ScNgroupsMax:
        case ScOpenMax:
        case ScSigqueueMax:
        case ScNprocessorsConf:
        case ScNprocessorsOnln:
        case ScPhysPages:
        case ScAvphysPages:
        case ScLevel1IcacheSize:
        case ScLevel1IcacheAssoc:
        case ScLevel1IcacheLinesize:
        case ScLevel1DcacheSize:
        case ScLevel1DcacheAssoc:
        case ScLevel1DcacheLinesize:
        case ScLevel2CacheSize:
        case ScLevel2CacheAssoc:
        case ScLevel2CacheLinesize:
        case ScLevel3CacheSize:
        case ScLevel3CacheAssoc:
        case ScLevel3CacheLinesize:
        case ScLevel4CacheSize:
        case ScLevel4CacheAssoc:
        case ScLevel4CacheLinesize: return -2;
        // The four names glibc's own sysconf has no case for answer EINVAL like an unknown one.
        case ScEquivClassMax:
        case ScSsReplMax:
        case ScThreadRobustPrioInherit:
        case ScThreadRobustPrioProtect: break;
        default: break;
    }
    errno = EINVAL;
    return -1;
}

// The online or possible processors, from the file the kernel fills in ("0-15" or "0,2-3").
int CountCpuList(const char* path) {
    char text[512];
    if (ReadWholeFile(path, text, sizeof text) < 0) return -1;
    int count = 0;
    const char* at = text;
    while (*at) {
        const int first = ParseInt(at, -1);
        if (first < 0) break;
        while (*at >= '0' && *at <= '9') ++at;
        int last = first;
        if (*at == '-') {
            last = ParseInt(at + 1, first);
            ++at;
            while (*at >= '0' && *at <= '9') ++at;
        }
        count += last - first + 1;
        if (*at != ',') break;
        ++at;
    }
    return count > 0 ? count : -1;
}

// The processors this process may run on, for a kernel without the online list.
int AffinityCount() {
    unsigned long mask[16] = {};
    if (cordlibc::Call(__NR_sched_getaffinity, 0, (long)sizeof mask, mask) <= 0) return -1;
    int count = 0;
    for (unsigned long word : mask)
        for (; word; word &= word - 1) ++count;
    return count > 0 ? count : 1;
}

int ProcessorCount(const char* path) {
    const int count = CountCpuList(path);
    return count > 0 ? count : AffinityCount();
}

// The cache sizes from /sys, where the kernel publishes what CPUID said. glibc's x86 sysconf asks
// CPUID itself and leaves the level-1 instruction associativity unset; /sys has it, so that one name
// answers here where glibc's does not (the checks accept either).
struct Caches {
    long size[5][2] = {};       // [level][0 = data or unified, 1 = instruction]
    long assoc[5][2] = {};
    long linesize[5][2] = {};
    bool seen[5][2] = {};
};

long CacheNumber(int index, const char* name) {
    char path[80] = "/sys/devices/system/cpu/cpu0/cache/index";
    int at = (int)__builtin_strlen(path);
    if (index > 9) return -1;
    path[at++] = (char)('0' + index);
    path[at++] = '/';
    for (const char* from = name; *from; ++from) path[at++] = *from;
    path[at] = 0;
    char text[32];
    if (ReadWholeFile(path, text, sizeof text) < 0) return -1;
    return ParseInt(text, -1);
}

long CacheSize(int index) {
    char path[80] = "/sys/devices/system/cpu/cpu0/cache/index";
    int at = (int)__builtin_strlen(path);
    path[at++] = (char)('0' + index);
    path[at++] = '/';
    for (const char* from = "size"; *from; ++from) path[at++] = *from;
    path[at] = 0;
    char text[32];
    if (ReadWholeFile(path, text, sizeof text) < 0) return -1;
    long size = ParseInt(text, -1);
    if (size < 0) return -1;
    for (const char* p = text; *p; ++p) {
        if (*p == 'K') size *= 1024;
        else if (*p == 'M') size *= 1024 * 1024;
    }
    return size;
}

void ReadCaches(Caches& caches) {
    for (int index = 0; index < 10; ++index) {
        const long level = CacheNumber(index, "level");
        if (level < 1 || level > 4) continue;
        char type[16];
        bool instruction = false;
        char path[80] = "/sys/devices/system/cpu/cpu0/cache/index";
        int at = (int)__builtin_strlen(path);
        path[at++] = (char)('0' + index);
        path[at++] = '/';
        for (const char* from = "type"; *from; ++from) path[at++] = *from;
        path[at] = 0;
        if (ReadWholeFile(path, type, sizeof type) >= 0 && type[0] == 'I') instruction = true;
        const int slot = instruction ? 1 : 0;
        const long size = CacheSize(index);
        if (size > 0) caches.size[level][slot] += size;
        const long assoc = CacheNumber(index, "ways_of_associativity");
        const long line = CacheNumber(index, "coherency_line_size");
        if (!caches.seen[level][slot]) {
            caches.assoc[level][slot] = assoc;
            caches.linesize[level][slot] = line;
        }
        caches.seen[level][slot] = true;
    }
}

bool CacheName(int name, int& level, int& slot, int& field) {
    switch (name) {
        case ScLevel1DcacheSize: level = 1; slot = 0; field = 0; return true;
        case ScLevel1IcacheSize: level = 1; slot = 1; field = 0; return true;
        case ScLevel2CacheSize: level = 2; slot = 0; field = 0; return true;
        case ScLevel3CacheSize: level = 3; slot = 0; field = 0; return true;
        case ScLevel4CacheSize: level = 4; slot = 0; field = 0; return true;
        case ScLevel1DcacheAssoc: level = 1; slot = 0; field = 1; return true;
        case ScLevel1IcacheAssoc: level = 1; slot = 1; field = 1; return true;
        case ScLevel2CacheAssoc: level = 2; slot = 0; field = 1; return true;
        case ScLevel3CacheAssoc: level = 3; slot = 0; field = 1; return true;
        case ScLevel4CacheAssoc: level = 4; slot = 0; field = 1; return true;
        case ScLevel1DcacheLinesize: level = 1; slot = 0; field = 2; return true;
        case ScLevel1IcacheLinesize: level = 1; slot = 1; field = 2; return true;
        case ScLevel2CacheLinesize: level = 2; slot = 0; field = 2; return true;
        case ScLevel3CacheLinesize: level = 3; slot = 0; field = 2; return true;
        case ScLevel4CacheLinesize: level = 4; slot = 0; field = 2; return true;
        default: return false;
    }
}

// The kernel's smallest signal stack, from AT_MINSIGSTKSZ (51) in the auxiliary vector. Kernels
// before 5.14 don't report it; 2048 is glibc's own default there.
int MinimumSignalStack() { return (int)Auxval(51, 2048); }

}  // namespace

CL long sysconf(int name) {
    const long constant = ConstantSc(name);
    if (constant != -2) return constant;
    switch (name) {
        case ScArgMax: {
            unsigned long stack = 0;
            if (!ReadLimit(kRlimitStack, stack)) return 2097152;  // 8 MiB, glibc's own default
            if (stack == ~0UL) return 6 * 1024 * 1024;           // glibc's answer for an unlimited stack
            return (long)(stack / 4);
        }
        case ScChildMax: {
            unsigned long limit = 0;
            return ReadLimit(kRlimitNproc, limit) && limit != ~0UL ? (long)limit : -1;
        }
        case ScSigqueueMax: {
            unsigned long limit = 0;
            return ReadLimit(kRlimitSigPending, limit) && limit != ~0UL ? (long)limit : -1;
        }
        case ScOpenMax: {
            unsigned long limit = 0;
            return ReadLimit(kRlimitNoFile, limit) && limit != ~0UL ? (long)limit : -1;
        }
        case ScNgroupsMax: {
            char text[32];
            if (ReadWholeFile("/proc/sys/kernel/ngroups_max", text, sizeof text) < 0) return 65536;
            return ParseInt(text, 65536);
        }
        case ScNprocessorsConf: return ProcessorCount("/sys/devices/system/cpu/possible");
        case ScNprocessorsOnln: return ProcessorCount("/sys/devices/system/cpu/online");
        case ScPagesize: return Auxval(6, 4096);  // AT_PAGESZ
        case ScPhysPages:
        case ScAvphysPages: {
            char text[4096];
            int kilobytes = -1;
            if (ReadWholeFile("/proc/meminfo", text, sizeof text) < 0) return -1;
            const char* label = name == ScPhysPages ? "MemTotal:" : "MemAvailable:";
            const bool found = ReadNumberAfter(text, label, kilobytes);
            return found ? kilobytes / 4 : -1;  // 4 KiB pages
        }
        case ScMinsigstksz: return MinimumSignalStack();
        case ScSigstksz: {
            const int minimum = MinimumSignalStack();
            return minimum > 8192 ? minimum : 8192;  // glibc's SIGSTKSZ on x86-64 is 8192
        }
        default: break;
    }
    int level = 0, slot = 0, field = 0;
    if (CacheName(name, level, slot, field)) {
        Caches caches;
        ReadCaches(caches);
        if (!caches.seen[level][slot]) return field == 0 ? 0 : -1;
        if (field == 0) return caches.size[level][slot];
        return field == 1 ? caches.assoc[level][slot] : caches.linesize[level][slot];
    }
    errno = EINVAL;
    return -1;
}

// The processor count libstdc++'s std::thread::hardware_concurrency asks for: glibc's get_nprocs is
// the online count, and the affinity mask without the list.
CL int get_nprocs(void) { return ProcessorCount("/sys/devices/system/cpu/online"); }

// -------------------------------------------------------------------------------------------------
// getpwuid: /etc/passwd, the source glibc's `files` entry reads. A user from another source (LDAP,
// systemd's dynamic users) is not found here, which is the layer's known limit, the same one stage
// 6's host lookups have. glibc answers a miss with a null pointer and errno cleared, and a hit from
// one static entry that the next call overwrites; both are the same here.

namespace {

// glibc's struct passwd (bits/passwd.h).
struct Passwd {
    char* name;
    char* password;
    unsigned uid;
    unsigned gid;
    char* gecos;
    char* directory;
    char* shell;
};

constexpr int kLine = 2048;   // one passwd line, fields included
constexpr int kChunk = 4096;

Passwd gPasswd;
char gPasswdLine[kLine];

// The uid field of one line, or false when the line is malformed or another user's.
bool ParsePasswdLine(char* line, unsigned wanted) {
    char* fields[7] = {};
    int count = 0;
    fields[count++] = line;
    for (char* at = line; *at && count < 7; ++at) {
        if (*at != ':') continue;
        *at = 0;
        fields[count++] = at + 1;
    }
    if (count < 7) return false;
    unsigned uid = 0;
    bool any = false;
    for (const char* at = fields[2]; *at >= '0' && *at <= '9'; ++at) {
        uid = uid * 10 + (unsigned)(*at - '0');
        any = true;
    }
    if (!any || uid != wanted) return false;
    unsigned gid = 0;
    for (const char* at = fields[3]; *at >= '0' && *at <= '9'; ++at) gid = gid * 10 + (unsigned)(*at - '0');
    gPasswd.name = fields[0];
    gPasswd.password = fields[1];
    gPasswd.uid = uid;
    gPasswd.gid = gid;
    gPasswd.gecos = fields[4];
    gPasswd.directory = fields[5];
    gPasswd.shell = fields[6];
    return true;
}

}  // namespace

CL Passwd* getpwuid(unsigned uid) {
    const int fd = (int)cordlibc::Call(__NR_openat, kAtFdCwd, "/etc/passwd", kReadOnly, 0);
    if (fd < 0) return nullptr;  // an unreadable file keeps its errno, as glibc's failure would
    char chunk[kChunk];
    int length = 0;
    bool found = false;
    for (;;) {
        const long got = cordlibc::Call(__NR_read, fd, chunk + length, (unsigned)(kChunk - length));
        if (got <= 0) break;
        length += (int)got;
        int start = 0;
        for (int at = 0; at < length; ++at) {
            if (chunk[at] != '\n') continue;
            chunk[at] = 0;
            const int lineLength = at - start;
            if (!found && lineLength < kLine) {
                for (int i = 0; i <= lineLength; ++i) gPasswdLine[i] = chunk[start + i];
                found = ParsePasswdLine(gPasswdLine, uid);
            }
            start = at + 1;
        }
        if (start) {
            for (int i = 0; i < length - start; ++i) chunk[i] = chunk[start + i];
            length -= start;
        }
        if (length == kChunk) length = 0;  // a line longer than the chunk is skipped
    }
    cordlibc::Call(__NR_close, fd);
    errno = 0;  // glibc's files source clears it, both for a hit and for a miss
    return found ? &gPasswd : nullptr;
}

// -------------------------------------------------------------------------------------------------
// nice: glibc's answer without a system call of its own (x86-64 has none): the kernel's priority is
// asked, raised or lowered, and asked again, so the answer is the new niceness. Raising it to -1
// cannot be told from failure, which is glibc's own ambiguity; nothing in cordinal asks for -1
// (pw_native asks for -10 and ignores the answer).

namespace {

constexpr int kPrioProcess = 0;

// The kernel's getpriority answers 20 - niceness, which is the conversion glibc's getpriority makes.
int CurrentNice() {
    const long value = cordlibc::Call(__NR_getpriority, kPrioProcess, 0L);
    if (value < 0) return -1;
    return 20 - (int)value;
}

}  // namespace

CL int nice(int increment) {
    const int current = CurrentNice();
    if (current < 0) return -1;
    if (cordlibc::Call(__NR_setpriority, kPrioProcess, 0L, (long)(current + increment)) < 0) return -1;
    return CurrentNice();
}


// -------------------------------------------------------------------------------------------------
// shmctl: the kernel's call under glibc's ABI. On x86-64 glibc's struct shmid_ds is the kernel's
// shmid64_ds field for field (misc_checks.h checks the 112 bytes and every field's offset against
// <sys/shm.h>), so the caller's buffer passes straight through. The kernel's IPC_64 bit is not added:
// x86-64 rejects that, as the checks confirm. The program's one call is X11's IPC_RMID.

namespace {

// glibc's struct ipc_perm and struct shmid_ds (bits/ipc.h, bits/shm.h).
struct IpcPerm {
    int key;
    unsigned uid, gid, cuid, cgid;
    unsigned mode;
    unsigned short seq;
    unsigned short pad;
    unsigned long reserved1, reserved2;
};

struct ShmDs {
    IpcPerm perm;
    unsigned long size;
    long atime, dtime, ctime;
    int cpid, lpid;
    unsigned long nattch;
    unsigned long unused4, unused5;
};

}  // namespace

CL int shmctl(int id, int command, ShmDs* buffer) {
    return (int)cordlibc::Call(__NR_shmctl, id, command, buffer);
}

// -------------------------------------------------------------------------------------------------
// fnmatch: musl's matcher (musl src/fnmatch.c, MIT, read November 18, 2026) with glibc's answers. The
// flags are glibc's: FNM_FILE_NAME is FNM_PATHNAME, and FNM_EXTMATCH's ksh patterns are not
// implemented since nothing asks for them. A bracket with no closing ']' is the literal it starts
// with, as glibc treats it.


namespace {

constexpr int kFnmPathname = 1;
constexpr int kFnmNoescape = 2;
constexpr int kFnmPeriod = 4;
constexpr int kFnmLeadingDir = 8;
constexpr int kFnmCasefold = 16;

int Fold(int c, int flags) { return (flags & kFnmCasefold) && c >= 'A' && c <= 'Z' ? c + 32 : c; }

// The C locale's character classes, which is what glibc's C-locale fnmatch tests.
bool IsAlnum(unsigned char v) {
    return (v >= 'a' && v <= 'z') || (v >= 'A' && v <= 'Z') || (v >= '0' && v <= '9');
}

bool InClass(const char* name, int length, unsigned char c) {
    struct Entry {
        const char* name;
        int length;
        bool (*test)(unsigned char);
    };
    static const Entry kEntries[] = {
        {"alpha", 5, [](unsigned char v) { return (v >= 'a' && v <= 'z') || (v >= 'A' && v <= 'Z'); }},
        {"digit", 5, [](unsigned char v) { return v >= '0' && v <= '9'; }},
        {"alnum", 5, IsAlnum},
        {"upper", 5, [](unsigned char v) { return v >= 'A' && v <= 'Z'; }},
        {"lower", 5, [](unsigned char v) { return v >= 'a' && v <= 'z'; }},
        {"space", 5, [](unsigned char v) { return v == ' ' || (v >= '\t' && v <= '\r'); }},
        {"blank", 5, [](unsigned char v) { return v == ' ' || v == '\t'; }},
        {"punct", 5,
         [](unsigned char v) {
             return v > 32 && v < 127 && !((v >= 'a' && v <= 'z') || (v >= 'A' && v <= 'Z') || (v >= '0' && v <= '9'));
         }},
        {"print", 5, [](unsigned char v) { return v >= 32 && v < 127; }},
        {"graph", 5, [](unsigned char v) { return v > 32 && v < 127; }},
        {"cntrl", 5, [](unsigned char v) { return v < 32 || v == 127; }},
        {"xdigit", 6,
         [](unsigned char v) {
             return (v >= '0' && v <= '9') || (v >= 'a' && v <= 'f') || (v >= 'A' && v <= 'F');
         }},
    };
    for (const Entry& entry : kEntries) {
        if (entry.length != length) continue;
        int i = 0;
        while (i < length && entry.name[i] == name[i]) ++i;
        if (i == length) return entry.test(c);
    }
    return false;
}

// The character-class half of [[:name:]]: 1 when the class is known (and tells whether c is in it),
// -1 when the name is not one glibc knows.
int MatchClass(const char* name, int length, unsigned char c) {
    static const char* const kClasses[] = {"alpha", "digit", "alnum",  "upper", "lower", "space",
                                           "blank", "punct", "print",  "graph", "cntrl", "xdigit"};
    for (const char* known : kClasses) {
        int i = 0;
        while (i < length && known[i] == name[i]) ++i;
        if (i == length && !known[i]) return InClass(name, length, c) ? 1 : 0;
    }
    return -1;
}

// pattern points at '['. On a match *next is past the closing ']'; the answer is 1 for a match, 0 for
// none, and -1 for a bracket with no closing ']' (which glibc takes as the literal it starts with).
int MatchBracket(const char* pattern, unsigned char c, int flags, const char** next) {
    const char* p = pattern + 1;
    bool negate = false;
    if (*p == '!' || *p == '^') {
        negate = true;
        ++p;
    }
    bool matched = false;
    bool first = true;
    for (;;) {
        if (!*p) return -1;
        if (*p == ']' && !first) {
            ++p;
            break;
        }
        first = false;
        if (*p == '[' && (p[1] == ':' || p[1] == '.' || p[1] == '=')) {
            const char closer = p[1];
            const char* name = p + 2;
            const char* end = name;
            while (*end && *end != closer) ++end;
            if (end[0] != closer || end[1] != ']') return -1;
            const int length = (int)(end - name);
            if (closer == ':') {
                const int result = MatchClass(name, length, c);
                if (result < 0) return -1;
                if (result) matched = true;
            } else {
                // A collating element ([.x.]) or an equivalence class ([=x=]): the C locale has one
                // character per symbol, so both are that character.
                if (length == 1 && Fold((unsigned char)name[0], flags) == Fold(c, flags)) matched = true;
                else if (length != 1) return -1;
            }
            p = end + 2;
            continue;
        }
        unsigned char low;
        if (*p == '\\' && !(flags & kFnmNoescape)) {
            if (!p[1]) return -1;
            low = (unsigned char)p[1];
            p += 2;
        } else {
            low = (unsigned char)*p++;
        }
        if (*p == '-' && p[1] && p[1] != ']') {
            ++p;
            unsigned char high;
            if (*p == '\\' && !(flags & kFnmNoescape)) {
                if (!p[1]) return -1;
                high = (unsigned char)p[1];
                p += 2;
            } else {
                high = (unsigned char)*p++;
            }
            if (high < low) return -1;
            const unsigned char folded = (unsigned char)Fold(c, flags);
            if ((folded >= Fold(low, flags) && folded <= Fold(high, flags)) || (c >= low && c <= high)) {
                matched = true;
            }
        } else if (Fold(low, flags) == Fold(c, flags)) {
            matched = true;
        }
    }
    *next = p;
    return negate ? !matched : matched;
}

// Whether a period here is one FNM_PERIOD protects: the string's first character, or the one after a
// slash under FNM_PATHNAME.
bool PeriodHere(const char* at, const char* start, int flags) {
    return (flags & kFnmPeriod) && *at == '.' && (at == start || ((flags & kFnmPathname) && at[-1] == '/'));
}

bool MatchHere(const char* pattern, const char* name, const char* start, int flags) {
    for (;;) {
        const char pc = *pattern;
        const unsigned char c = (unsigned char)*name;
        if (!pc) return c == 0 || ((flags & kFnmLeadingDir) && c == '/');
        if ((flags & kFnmPathname) && c == '/' && pc != '/') return false;
        if (pc == '*') {
            while (*pattern == '*') ++pattern;
            if (!*pattern) {
                // A star to the end of the pattern: everything left, but no slash under FNM_PATHNAME
                // and not a period FNM_PERIOD protects.
                if (PeriodHere(name, start, flags)) return false;
                return !((flags & kFnmPathname) && __builtin_strchr(name, '/'));
            }
            for (const char* at = name;; ++at) {
                if (!PeriodHere(at, start, flags) || at == name) {
                    if (MatchHere(pattern, at, start, flags)) return true;
                }
                if (!*at) return false;
                if ((flags & kFnmPathname) && *at == '/') return false;  // a star never crosses a slash
                if (PeriodHere(at, start, flags)) return false;          // nor takes a protected period
            }
        }
        if (!c) return false;
        if (pc == '?') {
            if (PeriodHere(name, start, flags)) return false;
            ++pattern;
            ++name;
            continue;
        }
        if (pc == '[') {
            if (PeriodHere(name, start, flags)) return false;
            const char* next = nullptr;
            const int answer = MatchBracket(pattern, c, flags, &next);
            if (answer < 0) {
                // No closing ']': the '[' is a literal, as glibc takes it.
                if (c != '[') return false;
                ++pattern;
                ++name;
                continue;
            }
            if (!answer) return false;
            pattern = next;
            ++name;
            continue;
        }
        if (pc == '\\' && !(flags & kFnmNoescape)) {
            if (!pattern[1]) return false;  // a trailing escape matches nothing
            ++pattern;
            if (Fold((unsigned char)*pattern, flags) != Fold(c, flags)) return false;
        } else if (Fold((unsigned char)pc, flags) != Fold(c, flags)) {
            return false;
        }
        ++pattern;
        ++name;
    }
}

}  // namespace

CL int fnmatch(const char* pattern, const char* name, int flags) {
    return MatchHere(pattern, name, name, flags) ? 0 : 1;  // glibc's FNM_NOMATCH is 1
}

// -------------------------------------------------------------------------------------------------
// qsort_r: glibc's GNU form (the context last, not BSD's argument order). A stable merge sort, which
// is what glibc's own sort is, with an insertion sort for short runs and an in-place quicksort when
// the temporary memory cannot be had.

namespace {

using Compare = int (*)(const void*, const void*, void*);

bool Before(const char* a, const char* b, Compare compare, void* argument) { return compare(a, b, argument) < 0; }

void Swap(char* a, char* b, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        const char t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

void InsertionSort(char* base, size_t count, size_t size, Compare compare, void* argument) {
    for (size_t i = 1; i < count; ++i) {
        for (size_t j = i; j > 0 && Before(base + j * size, base + (j - 1) * size, compare, argument); --j) {
            Swap(base + j * size, base + (j - 1) * size, size);
        }
    }
}

// The merge that keeps equal elements in order: a pass over runs of `width`.
void MergePass(char* source, char* target, size_t count, size_t size, size_t width, Compare compare, void* argument) {
    for (size_t start = 0; start < count; start += 2 * width) {
        const size_t middle = start + width < count ? start + width : count;
        const size_t end = start + 2 * width < count ? start + 2 * width : count;
        size_t left = start, right = middle, out = start;
        while (left < middle && right < end) {
            if (Before(source + right * size, source + left * size, compare, argument)) {
                for (size_t i = 0; i < size; ++i) target[out * size + i] = source[right * size + i];
                ++right;
            } else {
                for (size_t i = 0; i < size; ++i) target[out * size + i] = source[left * size + i];
                ++left;
            }
            ++out;
        }
        for (; left < middle; ++left, ++out)
            for (size_t i = 0; i < size; ++i) target[out * size + i] = source[left * size + i];
        for (; right < end; ++right, ++out)
            for (size_t i = 0; i < size; ++i) target[out * size + i] = source[right * size + i];
    }
}

void QuickSort(char* base, size_t count, size_t size, Compare compare, void* argument) {
    while (count > 12) {
        size_t middle = count / 2, last = count - 1;
        if (Before(base + middle * size, base, compare, argument)) Swap(base + middle * size, base, size);
        if (Before(base + last * size, base, compare, argument)) Swap(base + last * size, base, size);
        if (Before(base + last * size, base + middle * size, compare, argument)) Swap(base + last * size, base + middle * size, size);
        Swap(base + middle * size, base + (count - 2) * size, size);
        char* pivot = base + (count - 2) * size;
        size_t i = 0, j = count - 2;
        for (;;) {
            while (Before(base + (++i) * size, pivot, compare, argument)) {}
            while (j > 0 && Before(pivot, base + (--j) * size, compare, argument)) {}
            if (i >= j) break;
            Swap(base + i * size, base + j * size, size);
        }
        Swap(base + i * size, base + (count - 2) * size, size);
        const size_t left = i;
        QuickSort(base, left, size, compare, argument);
        base += (i + 1) * size;
        count -= i + 1;
    }
    InsertionSort(base, count, size, compare, argument);
}

}  // namespace

CL void qsort_r(void* base, size_t count, size_t size, Compare compare, void* argument) {
    if (count < 2 || size == 0 || !compare) return;
    char* const origin = (char*)base;
    if (count <= 16) {
        InsertionSort(origin, count, size, compare, argument);
        return;
    }
    char* const buffer = (char*)malloc(count * size);
    if (!buffer) {
        QuickSort(origin, count, size, compare, argument);
        return;
    }
    char* source = origin;
    char* target = buffer;
    for (size_t width = 1; width < count; width *= 2) {
        MergePass(source, target, count, size, width, compare, argument);
        char* const done = source;
        source = target;
        target = done;
    }
    if (source != origin) {
        for (size_t i = 0; i < count * size; ++i) origin[i] = source[i];
    }
    free(buffer);
}

// -------------------------------------------------------------------------------------------------
// wcscoll_l: the C locale's collation (locale.cpp), which is wide-character order whatever locale
// object is passed: the program's own locale objects carry no collation.

extern "C" __attribute__((visibility("hidden"))) int __wcscoll_l(const wchar_t*, const wchar_t*, void*);

CL int wcscoll_l(const wchar_t* first, const wchar_t* second, void* locale) {
    return __wcscoll_l(first, second, locale);
}

// -------------------------------------------------------------------------------------------------
// signal: glibc's BSD form, a sigaction with SA_RESTART. The kernel needs a restorer on x86-64 for a
// handler to return, which is the three instructions below.

extern "C" void cordinal_signal_return();
__asm__(".pushsection .text\n"
        ".hidden cordinal_signal_return\n"
        ".type cordinal_signal_return,@function\n"
        "cordinal_signal_return:\n"
        "movl $15, %eax\n"  // __NR_rt_sigreturn
        "syscall\n"
        ".size cordinal_signal_return, .-cordinal_signal_return\n"
        ".popsection\n");

namespace {

// The kernel's struct sigaction (asm/signal.h): handler, flags, restorer, then the mask.
struct KernelSigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask;
};

constexpr unsigned long kSaRestart = 0x10000000;
constexpr unsigned long kSaRestorer = 0x04000000;

}  // namespace

CL void (*signal(int number, void (*handler)(int)))(int) {
    KernelSigaction action{}, previous{};
    action.handler = handler;
    action.flags = kSaRestart | kSaRestorer;
    action.restorer = cordinal_signal_return;
    if (cordlibc::Call(__NR_rt_sigaction, number, &action, &previous, 8L) < 0) return (void (*)(int)) -1;
    return previous.handler;
}

// -------------------------------------------------------------------------------------------------
// arc4random: random bytes from the kernel, which is what the wrap in glibc_compat.cpp took from
// glibc 2.36 for std::random_device. Its own copy is the layer's now, and no kernel randomness is
// fatal, as glibc's is.

CL uint32_t arc4random(void) {
    uint32_t value = 0;
    unsigned char* bytes = (unsigned char*)&value;
    size_t got = 0;
    while (got < sizeof value) {
        const long n = cordlibc::Call(__NR_getrandom, bytes + got, (long)(sizeof value - got), 0L);
        if (n > 0) got += (size_t)n;
        else if (errno != EINTR) abort();
    }
    return value;
}

// -------------------------------------------------------------------------------------------------
// The gettext calls libstdc++'s messages and locale code link in. Nothing in the program translates:
// the C locale has one set of strings, so a lookup answers its message unchanged, and the domain
// calls store what they are given and answer it. The program stays in English.

namespace {

struct Domain {
    const char* name;
    const char* directory;
    const char* codeset;
};

constexpr int kDomains = 8;
Domain gDomains[kDomains];
int gDomainCount = 0;

const char* const kDefaultDirectory = "/usr/share/locale";

Domain* FindDomain(const char* name) {
    for (int i = 0; i < gDomainCount; ++i) {
        if (__builtin_strcmp(gDomains[i].name, name) == 0) return &gDomains[i];
    }
    return nullptr;
}

Domain* AddDomain(const char* name) {
    if (gDomainCount == kDomains) return nullptr;
    Domain& domain = gDomains[gDomainCount++];
    domain.name = name;
    domain.directory = nullptr;
    domain.codeset = nullptr;
    return &domain;
}

}  // namespace

CL char* gettext(const char* message) { return const_cast<char*>(message); }

CL char* dgettext(const char* domain, const char* message) {
    (void)domain;
    return const_cast<char*>(message);
}

CL char* bindtextdomain(const char* domain, const char* directory) {
    if (!domain || !*domain) return nullptr;  // glibc refuses a null or empty domain
    Domain* found = FindDomain(domain);
    if (directory) {
        if (!found) found = AddDomain(domain);
        if (!found) return nullptr;
        found->directory = directory;
        return const_cast<char*>(directory);
    }
    return const_cast<char*>(found && found->directory ? found->directory : kDefaultDirectory);
}

CL char* bind_textdomain_codeset(const char* domain, const char* codeset) {
    if (!domain || !*domain) return nullptr;
    Domain* found = FindDomain(domain);
    if (codeset) {
        if (!found) found = AddDomain(domain);
        if (!found) return nullptr;
        found->codeset = codeset;
        return const_cast<char*>(codeset);
    }
    return found ? const_cast<char*>(found->codeset) : nullptr;
}

// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------
// strfromf128 and strtof128: binary128's decimal conversions, which libstdc++'s std::to_chars and
// std::from_chars ask for (nothing in the program calls them). Both are exact rather than rounded
// twice: a value is written out from its own digits, held in base-10^9 limbs, and a parsed numeral is
// rounded once, to nearest with ties to even, from the value its digits and exponent spell.

namespace {

constexpr int kSignificandBits = 113;  // the fraction and the hidden bit
constexpr int kFractionBits = 112;
constexpr int kExponentBias = 16383;
constexpr int kMaximumExponent = 0x7fff;
constexpr int kSmallestQuantum = -16494;  // 2^(-16382) with 112 fraction bits, the smallest subnormal
constexpr int kMaxDigits = 11560;         // the smallest subnormal's digits number 11,529
constexpr int kMaxLimbs = kMaxDigits / 9 + 2;
constexpr int kMaxText = kMaxDigits + 8192;  // a %f body with a precision of up to 8,000 places

unsigned __int128 PowerOfTwo(int bits) { return (unsigned __int128)1 << bits; }

int BitLength128(unsigned __int128 value) {
    int bits = 0;
    while (value) {
        value >>= 1;
        ++bits;
    }
    return bits;
}

bool AnyNonzero(const char* digits, int from, int length) {
    for (int i = from; i < length; ++i)
        if (digits[i] != '0') return true;
    return false;
}

// A binary128 value taken apart: value = significand * 2^exponent.
struct Parts {
    bool negative;
    unsigned __int128 significand;
    int exponent;
    bool infinity, nan;
};

Parts TakeApart(_Float128 value) {
    union {
        _Float128 value;
        struct {
            uint64_t low, high;
        } words;
    } bits;
    bits.value = value;
    Parts parts{};
    parts.negative = (bits.words.high >> 63) != 0;
    const int field = (int)((bits.words.high >> 48) & 0x7fff);
    const unsigned __int128 fraction =
        ((unsigned __int128)(bits.words.high & 0xffffffffffffULL) << 64) | bits.words.low;
    if (field == kMaximumExponent) {
        parts.infinity = fraction == 0;
        parts.nan = fraction != 0;
        return parts;
    }
    if (field == 0) {
        parts.significand = fraction;
        parts.exponent = 1 - kExponentBias - kFractionBits;
    } else {
        parts.significand = fraction | PowerOfTwo(kFractionBits);
        parts.exponent = field - kExponentBias - kFractionBits;
    }
    return parts;
}

union FloatBits {
    _Float128 value;
    struct {
        uint64_t low, high;
    } words;
};

_Float128 Infinity(bool negative) {
    FloatBits bits{};
    bits.words.low = 0;
    bits.words.high = (negative ? 0x8000000000000000ULL : 0) | 0x7fff000000000000ULL;
    return bits.value;
}

// The closest binary128 value to significand * 2^exponent, ties to even, with `inexact` when any bit
// had to be dropped. A result too large answers an infinity.
_Float128 PutTogether(bool negative, unsigned __int128 significand, int exponent, bool& inexact) {
    FloatBits bits{};
    bits.words.low = 0;
    bits.words.high = negative ? 0x8000000000000000ULL : 0;
    inexact = false;
    if (!significand) return bits.value;
    int top = exponent + BitLength128(significand) - 1;  // the value is in [2^top, 2^(top+1))
    if (top + kExponentBias >= kMaximumExponent) {
        inexact = true;
        return Infinity(negative);
    }
    // The quantum the value sits on: the normal one, or the smallest subnormal's.
    const int unit = top + kExponentBias > 0 ? top - kFractionBits : kSmallestQuantum;
    const int shift = exponent - unit;
    unsigned __int128 kept;
    if (shift >= 0) {
        kept = significand << shift;
    } else {
        const int right = -shift;
        if (right >= 128) {
            kept = 0;
            inexact = true;
        } else {
            const bool guard = ((significand >> (right - 1)) & 1) != 0;
            const bool rest = right > 1 && (significand & (PowerOfTwo(right - 1) - 1)) != 0;
            kept = significand >> right;
            if (guard && (rest || (kept & 1))) ++kept;
            inexact = guard || rest;
        }
    }
    if (!kept) return bits.value;  // underflows to zero
    if (kept == PowerOfTwo(kFractionBits + 1)) {
        kept >>= 1;  // the rounding carried one bit up
        ++top;
    }
    uint64_t field = (uint64_t)(top + kExponentBias);
    if (top + kExponentBias <= 0) {
        field = 0;
        if (kept >= PowerOfTwo(kFractionBits)) {
            field = 1;  // rounds up to the smallest normal
            kept = 0;
        }
    }
    bits.words.high = (negative ? 0x8000000000000000ULL : 0) | (field << 48) |
                      ((uint64_t)(kept >> 64) & 0xffffffffffffULL);
    bits.words.low = (uint64_t)kept;
    return bits.value;
}

// A non-negative integer in base-10^9 limbs, little-endian: a value's exact decimal digits.
struct Decimal {
    uint32_t limb[kMaxLimbs];
    int count;

    void Set(unsigned __int128 value) {
        count = 0;
        do {
            limb[count++] = (uint32_t)(value % 1000000000u);
            value /= 1000000000u;
        } while (value);
    }

    void Mul(uint32_t factor) {
        uint64_t carry = 0;
        for (int i = 0; i < count; ++i) {
            const uint64_t product = (uint64_t)limb[i] * factor + carry;
            limb[i] = (uint32_t)(product % 1000000000u);
            carry = product / 1000000000u;
        }
        while (carry && count < kMaxLimbs) {
            limb[count++] = (uint32_t)(carry % 1000000000u);
            carry /= 1000000000u;
        }
    }

    void MulPowerOfTwo(int bits) {
        while (bits >= 29) {
            Mul(536870912u);
            bits -= 29;
        }
        if (bits) Mul(1u << bits);
    }

    void MulPowerOfFive(int five) {
        while (five >= 12) {
            Mul(244140625u);
            five -= 12;
        }
        while (five-- > 0) Mul(5);
    }

    // The digits, with no leading zeros ("0" for zero), into `out`, which holds kMaxDigits + 1.
    int Digits(char* out) const {
        int length = 0;
        for (int i = count - 1; i >= 0; --i) {
            char part[9];
            uint32_t value = limb[i];
            int done = 0;
            for (int k = 0; k < 9; ++k) {
                part[done++] = (char)('0' + value % 10);
                value /= 10;
            }
            int from = 8;
            if (i == count - 1)
                while (from > 0 && part[from] == '0') --from;
            for (int k = from; k >= 0; --k) out[length++] = part[k];
        }
        if (!length) out[length++] = '0';
        return length;
    }
};

// The exact decimal digits of a value: `digits` (no leading zeros), `length`, and `point`, the number
// of digits before the decimal point (zero or less when the value is below one).
void ExactDigits(const Parts& parts, char* digits, int& length, int& point) {
    if (!parts.significand) {
        digits[0] = '0';
        length = 1;
        point = 1;
        return;
    }
    Decimal decimal;
    decimal.Set(parts.significand);
    if (parts.exponent >= 0) {
        decimal.MulPowerOfTwo(parts.exponent);
        length = decimal.Digits(digits);
        point = length;
    } else {
        decimal.MulPowerOfFive(-parts.exponent);
        length = decimal.Digits(digits);
        point = length + parts.exponent;
    }
}

// Rounds the digits to their first `keep` (at least one), in place, and moves `point` when a carry
// makes the value ten times larger. `length` becomes keep, or stays shorter when nothing had to go.
void RoundInPlace(char* digits, int& length, int keep, int& point) {
    if (keep >= length) return;
    bool up = false;
    if (digits[keep] > '5') {
        up = true;
    } else if (digits[keep] == '5') {
        up = AnyNonzero(digits, keep + 1, length) || ((digits[keep - 1] - '0') & 1);
    }
    length = keep;
    if (!up) return;
    for (int i = keep - 1; i >= 0; --i) {
        if (digits[i] != '9') {
            ++digits[i];
            return;
        }
        digits[i] = '0';
    }
    digits[0] = '1';
    ++point;
}

// %f's body: the integer digits and `places` after the point. The rounding has already been done; an
// integer part that is not before the point is one zero, and missing fraction digits are zeros.
int FixedBody(const char* digits, int length, int point, int places, bool hash, char* out) {
    int written = 0;
    if (point <= 0) {
        out[written++] = '0';
    } else {
        for (int i = 0; i < point; ++i) out[written++] = i < length ? digits[i] : '0';
    }
    if (places || hash) out[written++] = '.';
    for (int i = 0; i < places; ++i) {
        const int index = point + i;
        out[written++] = index >= 0 && index < length ? digits[index] : '0';
    }
    return written;
}

// %e's body: one digit, a fraction of up to `places` digits (trailing zeros dropped when asked), and
// the exponent of ten, two digits at least.
int ExponentBody(const char* digits, int length, int point, int places, bool hash, bool stripZeros, bool upper,
                 char* out) {
    auto FractionDigit = [&](int i) { return i + 1 < length ? digits[i + 1] : '0'; };
    int count = places;
    if (stripZeros)
        while (count > 0 && FractionDigit(count - 1) == '0') --count;
    int written = 0;
    out[written++] = length ? digits[0] : '0';
    if (count || hash) {
        out[written++] = '.';
        for (int i = 0; i < count; ++i) out[written++] = FractionDigit(i);
    }
    const int exponent = point - 1;
    out[written++] = upper ? 'E' : 'e';
    out[written++] = exponent < 0 ? '-' : '+';
    unsigned magnitude = exponent < 0 ? (unsigned)-exponent : (unsigned)exponent;
    char text[8];
    int digitsOfExponent = 0;
    do {
        text[digitsOfExponent++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    while (digitsOfExponent < 2) text[digitsOfExponent++] = '0';
    while (digitsOfExponent) out[written++] = text[--digitsOfExponent];
    return written;
}

// %a's body: hexadecimal, from the value's own bits. A normal value leads with one; a subnormal leads
// with zero under the smallest normal exponent, which is glibc's own way of writing both. Without a
// precision the value is written exactly, its trailing zero digits dropped.
int HexBody(const Parts& parts, int places, bool stripZeros, bool hash, bool upper, char* out) {
    const bool zero = parts.significand == 0;
    const bool normal = (parts.significand >> kFractionBits) != 0;
    const unsigned __int128 fraction =
        normal ? parts.significand & (PowerOfTwo(kFractionBits) - 1) : parts.significand;
    const int exponent = zero ? 0 : normal ? parts.exponent + kFractionBits : 1 - kExponentBias;
    int digits[28];
    for (int i = 0; i < 28; ++i) digits[i] = (int)((fraction >> (kFractionBits - 4 * (i + 1))) & 0xf);
    // The rounding: the digit after the last printed one, then anything below it, and a tie to the
    // even digit, which for no fraction digits at all is the leading one (one is odd, zero even).
    int count = places;
    if (stripZeros) {
        while (count > 0 && count <= 28 && digits[count - 1] == 0) --count;
    }
    bool up = false;
    if (count < 28) {
        bool rest = false;
        for (int i = count + 1; i < 28; ++i)
            if (digits[i]) rest = true;
        const int last = count == 0 ? (normal ? 1 : 0) : digits[count - 1];
        up = digits[count] > 8 || (digits[count] == 8 && (rest || (last & 1)));
    }
    int lead = normal ? 1 : 0;
    if (up) {
        if (count == 0) {
            ++lead;  // 0x1.f rounds to 0x2, and a subnormal's zero to one
        } else {
            int i = count - 1;
            for (; i >= 0; --i) {
                if (digits[i] != 15) {
                    ++digits[i];
                    break;
                }
                digits[i] = 0;
            }
            if (i < 0) ++lead;
        }
    }
    int written = 0;
    out[written++] = '0';
    out[written++] = upper ? 'X' : 'x';
    out[written++] = (char)('0' + lead);
    if (count || hash) out[written++] = '.';
    for (int i = 0; i < count; ++i) {
        const int digit = i < 28 ? digits[i] : 0;
        out[written++] = (char)(digit < 10 ? '0' + digit : (upper ? 'A' : 'a') + digit - 10);
    }
    out[written++] = upper ? 'P' : 'p';
    out[written++] = exponent < 0 ? '-' : '+';
    unsigned magnitude = exponent < 0 ? (unsigned)-exponent : (unsigned)exponent;
    char text[8];
    int count2 = 0;
    do {
        text[count2++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    while (count2) out[written++] = text[--count2];
    return written;
}

// A non-negative integer in base 2^64 limbs, little-endian, for turning a numeral's digits into bits.
constexpr int kBinaryLimbs = 480;  // 30,720 bits: the widest numeral the range checks let through

struct Binary {
    uint64_t limb[kBinaryLimbs];
    int count;

    void Set(uint64_t value) {
        count = 1;
        limb[0] = value;
    }

    void SetBit(int bit) {
        const int index = bit / 64;
        if (index >= kBinaryLimbs) return;
        while (count <= index) limb[count++] = 0;
        limb[index] |= (uint64_t)1 << (bit % 64);
    }

    void Set128(unsigned __int128 value) {
        limb[0] = (uint64_t)value;
        limb[1] = (uint64_t)(value >> 64);
        count = limb[1] ? 2 : 1;
    }

    bool IsZero() const { return count == 1 && limb[0] == 0; }

    int Bits() const {
        int bits = (count - 1) * 64;
        for (uint64_t top = limb[count - 1]; top; top >>= 1) ++bits;
        return bits;
    }

    void Trim() {
        while (count > 1 && limb[count - 1] == 0) --count;
    }

    void MulSmall(uint64_t factor) {
        unsigned __int128 carry = 0;
        for (int i = 0; i < count; ++i) {
            const unsigned __int128 product = (unsigned __int128)limb[i] * factor + carry;
            limb[i] = (uint64_t)product;
            carry = product >> 64;
        }
        if (carry && count < kBinaryLimbs) limb[count++] = (uint64_t)carry;
    }

    void AddSmall(uint64_t value) {
        unsigned __int128 carry = value;
        for (int i = 0; i < count && carry; ++i) {
            const unsigned __int128 sum = (unsigned __int128)limb[i] + carry;
            limb[i] = (uint64_t)sum;
            carry = sum >> 64;
        }
        if (carry && count < kBinaryLimbs) limb[count++] = (uint64_t)carry;
    }

    void MulPowerOfFive(int five) {
        while (five >= 27) {
            MulSmall(7450580596923828125ULL);  // 5^27
            five -= 27;
        }
        while (five-- > 0) MulSmall(5);
    }

    void MulPowerOfTwo(int bits) {
        const int words = bits / 64;
        const int extra = bits % 64;
        if (words) {
            const int moved = count + words > kBinaryLimbs ? kBinaryLimbs - count : words;
            for (int i = count - 1; i >= 0; --i) {
                if (i + moved < kBinaryLimbs) limb[i + moved] = limb[i];
            }
            for (int i = 0; i < moved; ++i) limb[i] = 0;
            count += moved;
        }
        if (extra) MulSmall((uint64_t)1 << extra);
    }

    // Drops `bits` low bits, reporting whether any of them was set.
    bool ShiftRight(int bits) {
        const int words = bits / 64;
        const int extra = bits % 64;
        if (words >= count) {
            const bool dropped = !IsZero();
            Set(0);
            return dropped;
        }
        bool dropped = false;
        for (int i = 0; i < words; ++i)
            if (limb[i]) dropped = true;
        if (extra) dropped = dropped || (limb[words] & ((1ULL << extra) - 1)) != 0;
        for (int i = 0; i + words < count; ++i) {
            uint64_t value = limb[i + words] >> extra;
            if (extra && i + words + 1 < count) value |= limb[i + words + 1] << (64 - extra);
            limb[i] = value;
        }
        count -= words;
        Trim();
        return dropped;
    }

    int Compare(const Binary& other) const {
        if (count != other.count) return count < other.count ? -1 : 1;
        for (int i = count - 1; i >= 0; --i)
            if (limb[i] != other.limb[i]) return limb[i] < other.limb[i] ? -1 : 1;
        return 0;
    }

    void Subtract(const Binary& other) {
        uint64_t borrow = 0;
        for (int i = 0; i < count; ++i) {
            const uint64_t from = i < other.count ? other.limb[i] : 0;
            const uint64_t sum = from + borrow;
            const bool over = sum < from;
            const uint64_t low = limb[i] - sum;
            borrow = over || limb[i] < sum ? 1 : 0;
            limb[i] = low;
        }
        Trim();
    }

    Binary ShiftedLeft(int bits) const {
        Binary out = *this;
        out.MulPowerOfTwo(bits);
        return out;
    }

    unsigned __int128 Low() const { return count > 1 ? (unsigned __int128)limb[1] << 64 | limb[0] : limb[0]; }
};

// The quotient of two integers, kept to about 192 bits: q = floor(numerator * 2^shifted / divisor),
// with `sticky` when anything below the kept bits was nonzero.
Binary Divide(const Binary& numerator, const Binary& divisor, bool& sticky, int& shifted) {
    Binary left = numerator;
    sticky = false;
    shifted = 0;
    const int want = divisor.Bits() + 200 - left.Bits();
    if (want > 0) {
        left.MulPowerOfTwo(want);
        shifted = -want;
    } else if (want < 0) {
        sticky = left.ShiftRight(-want);
        shifted = -want;
    }
    Binary out;
    out.Set(0);
    int top = left.Bits() - divisor.Bits();
    if (top > 200) top = 200;
    for (int bit = top; bit >= 0; --bit) {
        const Binary shiftedDivisor = divisor.ShiftedLeft(bit);
        if (left.Compare(shiftedDivisor) >= 0) {
            left.Subtract(shiftedDivisor);
            out.SetBit(bit);
        }
    }
    sticky = sticky || !left.IsZero();
    return out;
}

// Rounds an integer to the significand's 113 bits, ties to even, adding the bits dropped to
// `exponent`; `sticky` is a nonzero remainder below the integer's lowest bit, and `dropped` comes back
// saying whether the rounding left the value inexact.
unsigned __int128 RoundSignificand(unsigned __int128 value, bool sticky, int& exponent, bool& dropped) {
    const int bits = BitLength128(value);
    dropped = false;
    if (bits <= kSignificandBits) return value;
    const int drop = bits - kSignificandBits;
    const bool guard = ((value >> (drop - 1)) & 1) != 0;
    const bool rest = drop > 1 && (value & (PowerOfTwo(drop - 1) - 1)) != 0;
    value >>= drop;
    if (guard && (rest || sticky || (value & 1))) ++value;
    int dropped2 = drop;
    if (BitLength128(value) > kSignificandBits) {
        value >>= 1;
        ++dropped2;
    }
    exponent += dropped2;
    dropped = guard || rest || sticky;
    return value;
}

// The significand's bits from a bignum's top, rounded to 113 bits: the bignum is cut down to 128 bits
// first (its own dropped bits joining the remainder), which leaves the guard bits in place.
unsigned __int128 BignumSignificand(Binary& value, bool sticky, int& exponent, bool& dropped) {
    const int bits = value.Bits();
    if (bits > 128) {
        const int drop = bits - 128;
        sticky = value.ShiftRight(drop) || sticky;
        exponent += drop;
    }
    return RoundSignificand(value.Low(), sticky, exponent, dropped);
}

bool IsSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// More significant digits than this are only a remainder: a binary128 value's own decimal digits
// never number beyond 4,000 and thirty-eight.
constexpr int kMaxSignificant = 4000;

// The value a numeral spells, rounded once to binary128. glibc's ERANGE comes with an infinity, and
// with a rounded result whose exact value sat below the smallest normal (a subnormal, a zero, or the
// smallest normal reached by rounding up from below it).
void NoteInexact(int exactTop, int& error) {
    if (exactTop + kExponentBias <= 0 || exactTop + kExponentBias >= kMaximumExponent) error = ERANGE;
}

_Float128 FromNumeral(const char* digits, int length, int exponent10, bool sticky, bool negative, int& error) {
    const int magnitude = exponent10 + length;  // the value is below 10^magnitude
    if (magnitude > 4934) {
        error = ERANGE;
        return Infinity(negative);
    }
    if (magnitude < -4967) {
        error = ERANGE;
        return PutTogether(negative, 0, 0, sticky);
    }
    Binary value;
    value.Set(0);
    for (int i = 0; i < length; ++i) {
        value.MulSmall(10);
        value.AddSmall((uint64_t)(digits[i] - '0'));
    }
    int exponent2 = exponent10;
    if (exponent10 < 0) {
        Binary divisor;
        divisor.Set(1);
        divisor.MulPowerOfFive(-exponent10);
        bool remainder = false;
        int shifted = 0;
        value = Divide(value, divisor, remainder, shifted);
        sticky = sticky || remainder;
        exponent2 += shifted;
    } else {
        value.MulPowerOfFive(exponent10);
    }
    bool rounded = false;
    const unsigned __int128 significand = BignumSignificand(value, sticky, exponent2, rounded);
    const int exactTop = significand ? exponent2 + BitLength128(significand) - 1 : -1000000;
    bool inexact = false;
    const _Float128 answer = PutTogether(negative, significand, exponent2, inexact);
    if (inexact || rounded) NoteInexact(exactTop, error);
    return answer;
}

// A decimal or hexadecimal numeral from `at` (which is past the sign); `at` moves to the end of what
// was read, and `converted` says whether anything was.
_Float128 ParseNumber(const char*& at, bool negative, bool& converted, int& error) {
    const bool hexadecimal = at[0] == '0' && (at[1] == 'x' || at[1] == 'X') &&
                             (HexDigit(at[2]) >= 0 || (at[2] == '.' && HexDigit(at[3]) >= 0));
    if (hexadecimal) {
        at += 2;
        unsigned __int128 significand = 0;
        int kept = 0;
        bool sticky = false;
        bool afterPoint = false;
        int fractionDigits = 0;
        for (;;) {
            const int digit = HexDigit(*at);
            if (digit >= 0) {
                if (kept < 29) {
                    significand = (significand << 4) | (unsigned __int128)digit;
                    ++kept;
                } else if (digit) {
                    sticky = true;
                }
                if (afterPoint) ++fractionDigits;
                ++at;
                continue;
            }
            if (*at == '.' && !afterPoint) {
                afterPoint = true;
                ++at;
                continue;
            }
            break;
        }
        int power = 0;
        if (*at == 'p' || *at == 'P') {
            const char* after = at + 1;
            const bool negativeExponent = *after == '-';
            if (*after == '+' || *after == '-') ++after;
            if (*after >= '0' && *after <= '9') {
                while (*after >= '0' && *after <= '9') {
                    if (power < 100000) power = power * 10 + (*after - '0');
                    ++after;
                }
                if (negativeExponent) power = -power;
                at = after;
            }
        }
        converted = true;
        if (!significand) {
            bool inexact = false;
            return PutTogether(negative, 0, 0, inexact);
        }
        int exponent2 = power - 4 * fractionDigits;
        int bits = kept * 4;
        while (bits > 1 && !((significand >> (bits - 1)) & 1)) --bits;
        const int exactTop = exponent2 + bits - 1;
        bool rounded = false;
        if (bits > kSignificandBits) {
            const int drop = bits - kSignificandBits;
            const bool guard = ((significand >> (drop - 1)) & 1) != 0;
            const bool rest = drop > 1 && (significand & (PowerOfTwo(drop - 1) - 1)) != 0;
            significand >>= drop;
            if (guard && (rest || sticky || (significand & 1))) ++significand;
            int dropped = drop;
            if (BitLength128(significand) > kSignificandBits) {
                significand >>= 1;
                ++dropped;
            }
            exponent2 += dropped;
            rounded = guard || rest || sticky;
        }
        bool inexact = false;
        const _Float128 answer = PutTogether(negative, significand, exponent2, inexact);
        if (inexact || rounded) NoteInexact(exactTop, error);
        return answer;
    }

    char digits[kMaxSignificant];
    int length = 0;
    int seen = 0;
    bool sticky = false;
    bool any = false;
    bool afterPoint = false;
    int fractionDigits = 0;
    for (;;) {
        const char c = *at;
        if (c >= '0' && c <= '9') {
            any = true;
            ++seen;
            if (length < kMaxSignificant) digits[length++] = c;
            else if (c != '0') sticky = true;  // beyond the buffer: only a remainder, and its place
            if (afterPoint) ++fractionDigits;
            ++at;
            continue;
        }
        if (c == '.' && !afterPoint) {
            afterPoint = true;
            ++at;
            continue;
        }
        break;
    }
    if (!any) {
        converted = false;
        return 0;
    }
    int exponent = 0;
    if (*at == 'e' || *at == 'E') {
        const char* after = at + 1;
        const bool negativeExponent = *after == '-';
        if (*after == '+' || *after == '-') ++after;
        if (*after >= '0' && *after <= '9') {
            while (*after >= '0' && *after <= '9') {
                if (exponent < 100000) exponent = exponent * 10 + (*after - '0');
                ++after;
            }
            if (negativeExponent) exponent = -exponent;
            at = after;
        }
    }
    converted = true;
    int first = 0;
    while (first < length && digits[first] == '0') ++first;
    if (first == length) {
        bool inexact = false;
        return PutTogether(negative, 0, 0, inexact);
    }
    return FromNumeral(digits + first, length - first, exponent - fractionDigits + (seen - length), sticky, negative,
                       error);
}

}  // namespace

CL _Float128 strtof128(const char* text, char** end) {
    const char* at = text;
    while (IsSpace(*at)) ++at;
    bool negative = false;
    if (*at == '+' || *at == '-') {
        negative = *at == '-';
        ++at;
    }
    bool converted = false;
    int error = 0;
    _Float128 answer = 0;
    int matched = 0;
    for (const char* word = "infinity"; word[matched] && (at[matched] | 32) == word[matched]; ++matched) {}
    if (matched >= 3) {
        at += matched >= 8 ? 8 : 3;  // "infinity" in full, otherwise "inf" and the rest unread
        answer = Infinity(negative);
        converted = true;
    } else {
        int nanLength = 0;
        for (const char* word = "nan"; word[nanLength] && (at[nanLength] | 32) == word[nanLength]; ++nanLength) {}
        if (nanLength == 3) {
            at += 3;
            // glibc keeps the n-char-sequence as the quiet NaN's low bits, read like strtoull's
            // base-0 number and saturated at 64 bits' worth.
            uint64_t payload = 0;
            if (*at == '(') {
                const char* close = at + 1;
                const char* digits = close;
                bool negativePayload = false;
                if (*digits == '+' || *digits == '-') {
                    negativePayload = *digits == '-';
                    ++digits;
                }
                int base = 10;
                if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
                    base = 16;
                    digits += 2;
                }
                bool any = false;
                for (; *digits; ++digits) {
                    const int digit = base == 16 ? HexDigit(*digits) : *digits >= '0' && *digits <= '9' ? *digits - '0' : -1;
                    if (digit < 0) break;
                    any = true;
                    if (payload > (~(uint64_t)0 - (uint64_t)digit) / (uint64_t)base) payload = ~(uint64_t)0;
                    else payload = payload * (uint64_t)base + (uint64_t)digit;
                }
                if (!any) payload = 0;
                if (negativePayload) payload = 0 - payload;
                while (*close && (HexDigit(*close) >= 0 || *close == '_' || *close == 'x' || *close == 'X' ||
                                   *close == '+' || *close == '-'))
                    ++close;
                if (*close == ')') at = close + 1;
            }
            FloatBits bits{};
            bits.words.low = payload;
            bits.words.high = 0x7fff800000000000ULL | (negative ? 0x8000000000000000ULL : 0);
            answer = bits.value;
            converted = true;
        } else {
            answer = ParseNumber(at, negative, converted, error);
        }
    }
    if (end) *end = const_cast<char*>(converted ? at : text);
    if (error) errno = error;
    return answer;
}

// -------------------------------------------------------------------------------------------------
// strfromf128: printf's %f, %e, %g and %a for a binary128 value. libstdc++'s std::to_chars asks for
// the first three with a precision; glibc's own strfromf128 reads little else (it ends the program on
// most other formats), and this answers -1 for a format it cannot read. The answer is the length the
// whole text has, with what fits of it and a terminator written, as snprintf's is.

namespace {

int WriteOut(char* out, size_t size, const char* text, int length) {
    if (size) {
        const size_t copied = (size_t)length < size - 1 ? (size_t)length : size - 1;
        for (size_t i = 0; i < copied; ++i) out[i] = text[i];
        out[copied] = 0;
    }
    return length;
}

}  // namespace

CL int strfromf128(char* out, size_t size, const char* format, _Float128 value) {
    if (!format || format[0] != '%') return -1;
    const char* at = format + 1;
    bool left = false, plus = false, space = false, hash = false, zero = false;
    for (;;) {
        if (*at == '-') left = true;
        else if (*at == '+') plus = true;
        else if (*at == ' ') space = true;
        else if (*at == '#') hash = true;
        else if (*at == '0') zero = true;
        else break;
        ++at;
    }
    int width = 0;
    while (*at >= '0' && *at <= '9') width = width * 10 + (*at++ - '0');
    int precision = -1;
    if (*at == '.') {
        ++at;
        precision = 0;
        while (*at >= '0' && *at <= '9') precision = precision * 10 + (*at++ - '0');
    }
    while (*at == 'l' || *at == 'L') ++at;  // a length modifier says nothing about a binary128 value
    const char conversion = *at;
    if (!conversion || at[1]) return -1;
    const bool upper = conversion >= 'A' && conversion <= 'Z';
    const char kind = (char)(conversion | 32);
    if (kind != 'f' && kind != 'e' && kind != 'g' && kind != 'a') return -1;
    if (precision > 7000 || width > 7000) return -1;
    if (kind != 'a' && precision < 0) precision = 6;

    const Parts parts = TakeApart(value);
    const char sign = parts.negative ? '-' : plus ? '+' : space ? ' ' : 0;
    char digits[kMaxDigits + 8];
    char text[kMaxText];
    int length = 0;
    int point = 0;
    if (parts.infinity || parts.nan) {
        const char* word = parts.nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        for (const char* from = word; *from; ++from) text[length++] = *from;
    } else {
        ExactDigits(parts, digits, length, point);
        if (kind == 'a') {
            if (precision < 0) {
                length = HexBody(parts, 28, true, hash, upper, text);
            } else {
                length = HexBody(parts, precision, false, hash, upper, text);
            }
        } else if (kind == 'f') {
            const int keep = point + precision;
            if (keep <= 0) {
                // Every digit sits below the printed places; only a carry could reach them.
                const bool up =
                    keep == 0 && (digits[0] > '5' || (digits[0] == '5' && AnyNonzero(digits, 1, length)));
                digits[0] = up ? '1' : '0';
                length = 1;
                point = up ? 1 - precision : 1;
            } else {
                RoundInPlace(digits, length, keep, point);
            }
            if (point > kMaxText - precision - 8) return -1;
            length = FixedBody(digits, length, point, precision, hash, text);
        } else if (kind == 'e') {
            RoundInPlace(digits, length, precision + 1, point);
            length = ExponentBody(digits, length, point, precision, hash, false, upper, text);
        } else {
            const int significant = precision == 0 ? 1 : precision;
            RoundInPlace(digits, length, significant, point);
            const int exponent = point - 1;
            if (exponent < -4 || exponent >= significant) {
                length = ExponentBody(digits, length, point, significant - 1, hash, !hash, upper, text);
            } else {
                const int places = significant - 1 - exponent;
                if (point > kMaxText - places - 8) return -1;
                length = FixedBody(digits, length, point, places, hash, text);
                if (!hash && places > 0) {
                    // The fraction's trailing zeros go, and the point with them; the integer part stays.
                    int end = length;
                    while (end > length - places && text[end - 1] == '0') --end;
                    if (end == length - places) --end;
                    length = end;
                }
            }
        }
    }
    const int total = (sign ? 1 : 0) + length;
    const int padding = width > total ? width - total : 0;
    if (total + padding >= kMaxText) return -1;
    // The sign, then the padding: zeros next to the sign for a right-aligned number.
    char line[kMaxText];
    int written = 0;
    if (left) {
        if (sign) line[written++] = sign;
        for (int i = 0; i < length; ++i) line[written++] = text[i];
        for (int i = 0; i < padding; ++i) line[written++] = ' ';
    } else if (zero && sign && !parts.infinity && !parts.nan) {
        line[written++] = sign;
        for (int i = 0; i < padding; ++i) line[written++] = '0';
        for (int i = 0; i < length; ++i) line[written++] = text[i];
    } else {
        for (int i = 0; i < padding; ++i) line[written++] = ' ';
        if (sign) line[written++] = sign;
        for (int i = 0; i < length; ++i) line[written++] = text[i];
    }
    return WriteOut(out, size, line, written);
}
