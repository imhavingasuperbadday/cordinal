// cordinal's streams, formatting and scanning (stdio.cpp, printf.cpp, scanf.cpp) against glibc's,
// for checks.h: a wide barrage of snprintf formats compared byte for byte with glibc's answer, the
// stream layer taking the same calls as glibc's FILE streams (buffering to a pipe, ungetc across
// buffer boundaries, seek and tell, the end-of-file and error flags, partial transfers, fflush,
// fflush(NULL), the flush at exit, getline and getdelim), and scanf's answers for the same inputs
// (whitespace, widths, %n, return counts, character classes, suppression, EOF against 0). glibc's
// own functions come back through dlsym, so each answer is compared with the one glibc gives rather
// than with a guess.
#pragma once
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>

#include <string>
#include <type_traits>
#include <vector>

namespace libc_checks {

// glibc's own stdio, for the comparisons. Its FILE streams stay entirely glibc's: every call on one
// goes through these pointers, never through the program's functions.
struct GlibcStdio {
    FILE* (*fopen)(const char*, const char*) = Glibc<FILE* (*)(const char*, const char*)>("fopen");
    FILE* (*fdopen)(int, const char*) = Glibc<FILE* (*)(int, const char*)>("fdopen");
    int (*fclose)(FILE*) = Glibc<int (*)(FILE*)>("fclose");
    size_t (*fread)(void*, size_t, size_t, FILE*) = Glibc<size_t (*)(void*, size_t, size_t, FILE*)>("fread");
    size_t (*fwrite)(const void*, size_t, size_t, FILE*) =
        Glibc<size_t (*)(const void*, size_t, size_t, FILE*)>("fwrite");
    int (*fseek)(FILE*, long, int) = Glibc<int (*)(FILE*, long, int)>("fseek");
    long (*ftell)(FILE*) = Glibc<long (*)(FILE*)>("ftell");
    int (*fflush)(FILE*) = Glibc<int (*)(FILE*)>("fflush");
    int (*fgetc)(FILE*) = Glibc<int (*)(FILE*)>("fgetc");
    int (*fputc)(int, FILE*) = Glibc<int (*)(int, FILE*)>("fputc");
    int (*ungetc)(int, FILE*) = Glibc<int (*)(int, FILE*)>("ungetc");
    int (*feof)(FILE*) = Glibc<int (*)(FILE*)>("feof");
    int (*ferror)(FILE*) = Glibc<int (*)(FILE*)>("ferror");
    int (*fileno)(FILE*) = Glibc<int (*)(FILE*)>("fileno");
    char* (*fgets)(char*, int, FILE*) = Glibc<char* (*)(char*, int, FILE*)>("fgets");
    int (*fputs)(const char*, FILE*) = Glibc<int (*)(const char*, FILE*)>("fputs");
    int (*setvbuf)(FILE*, char*, int, size_t) = Glibc<int (*)(FILE*, char*, int, size_t)>("setvbuf");
    long (*getdelim)(char**, size_t*, int, FILE*) =
        Glibc<long (*)(char**, size_t*, int, FILE*)>("getdelim");
    int (*remove)(const char*) = Glibc<int (*)(const char*)>("remove");
};
inline GlibcStdio& Gs() {
    static GlibcStdio glibc;
    return glibc;
}

// glibc's snprintf through vsnprintf, taking the caller's va_list (a variadic wrapper would pass
// its own arguments, which are the va_list itself).
inline int GlibcFormat(char* buffer, size_t capacity, const char* format, va_list ap) {
    static auto vsnprintf_ = Glibc<int (*)(char*, size_t, const char*, va_list)>("vsnprintf");
    return vsnprintf_ ? vsnprintf_(buffer, capacity, format, ap) : -1;
}

// The same for a single formatted call.
inline int GlibcPrintf(char* buffer, size_t capacity, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = GlibcFormat(buffer, capacity, format, ap);
    va_end(ap);
    return result;
}

inline int GlibcScan(const char* input, const char* format, ...) {
    static auto vsscanf_ = Glibc<int (*)(const char*, const char*, va_list)>("__isoc23_vsscanf");
    static auto vsscanfOld = Glibc<int (*)(const char*, const char*, va_list)>("vsscanf");
    auto scan = vsscanf_ ? vsscanf_ : vsscanfOld;
    if (!scan) return -99;
    va_list ap;
    va_start(ap, format);
    const int result = scan(input, format, ap);
    va_end(ap);
    return result;
}

inline int GlibcWideScan(const wchar_t* input, const wchar_t* format, ...) {
    static auto vswscanf_ = Glibc<int (*)(const wchar_t*, const wchar_t*, va_list)>("vswscanf");
    if (!vswscanf_) return -99;
    va_list ap;
    va_start(ap, format);
    const int result = vswscanf_(input, format, ap);
    va_end(ap);
    return result;
}

inline int GlibcAsprintf(char** output, const char* format, ...) {
    static auto vasprintf_ = Glibc<int (*)(char**, const char*, va_list)>("vasprintf");
    int result = -1;
    va_list ap;
    va_start(ap, format);
    if (vasprintf_) result = vasprintf_(output, format, ap);
    va_end(ap);
    return result;
}

// What a barrage found: the first mismatches are printed with both answers, and one Expect per group
// reports whether any were seen.
struct Barrage {
    bool clean = true;
    int shown = 0;
    const char* context = "";

    void Differ(const char* what, const char* mine, const char* theirs) {
        clean = false;
        if (shown < 12) {
            ++shown;
            fprintf(stderr, "  %s%s%s: cordinal %s, glibc %s\n", what, *context ? " of " : "", context,
                    mine, theirs);
        }
    }
};

inline void CompareFormat(Barrage& seen, const char* format, ...) {
    char mine[2048] = {}, theirs[2048] = {};
    va_list ap, copy;
    va_start(ap, format);
    va_copy(copy, ap);
    const int length = vsnprintf(mine, sizeof mine, format, ap);
    va_end(ap);
    const int theirLength = GlibcFormat(theirs, sizeof theirs, format, copy);
    va_end(copy);
    bool same = length == theirLength;
    if (same && length >= 0) {  // the bytes and the terminator, which is what a %c of 0 hides in
        const size_t compare = (size_t)length < sizeof mine ? (size_t)length + 1 : sizeof mine;
        same = !memcmp(mine, theirs, compare);
    }
    if (!same) {
        char what[256], a[512], b[512];
        snprintf(what, sizeof what, "\"%s\"", format);
        snprintf(a, sizeof a, "\"%s\" (%d)", mine, length);
        snprintf(b, sizeof b, "\"%s\" (%d)", theirs, theirLength);
        seen.Differ(what, a, b);
    }
}

// The same formats for a group of values: one line per group in the report.
template <class T>
inline void CompareFormats(Barrage& seen, const char* const* formats, size_t count, const T* values,
                           size_t valueCount) {
    for (size_t f = 0; f < count; ++f) {
        for (size_t v = 0; v < valueCount; ++v) {
            // The value goes into the report, so a difference names the case that made it.
            char context[64];
            if constexpr (std::is_floating_point_v<T>)
                snprintf(context, sizeof context, "%.21Lg", (long double)values[v]);
            else if constexpr (std::is_signed_v<T>)
                snprintf(context, sizeof context, "%lld", (long long)values[v]);
            else
                snprintf(context, sizeof context, "%llu", (unsigned long long)values[v]);
            seen.context = context;
            CompareFormat(seen, formats[f], values[v]);
        }
    }
    seen.context = "";
}

inline void CheckPrintf(const std::string& dir) {
    // The percent conversions, flags, widths and precisions, over values that reach the edges.
    static const char* const signedFormats[] = {
        "%d",    "%i",     "%5d",    "%-5d",   "%05d",    "%+d",   "% d",    "%+05d",
        "%-+5d", "% -5d",  "%.0d",   "%.1d",   "%.3d",    "%8.3d", "%-8.3d", "%08.3d",
        "%+.0d", "%-6.0d", "% 06.3d", "%12.7d", "%hhd",   "%hd",   "%.1hhd", "%6.2hd",
        "%.4i",  "%+hi",
    };
    static const int signedValues[] = {0, 1, -1, 42, -42, 127, -128, 32767, -32768, INT_MAX, INT_MIN};
    static const char* const unsignedFormats[] = {
        "%u",    "%5u",   "%-5u",  "%05u",  "%.0u",   "%.4u",   "%8.4u",  "%#o",    "%#5o",
        "%#.0o", "%#.0x", "%x",    "%X",    "%#x",    "%#X",    "%08x",   "%-8.4x", "%#12.6X",
        "%.0x",  "%hhu",  "%hu",   "%ho",   "%hx",    "%#12o",  "%20u",
    };
    static const unsigned unsignedValues[] = {0u, 1u, 7u, 255u, 3054u, 0xdeadbeefu, UINT_MAX};
    static const char* const longFormats[] = {
        "%ld", "%lx", "%lo", "%lu", "%li", "%20ld", "%-20ld", "%020ld", "%.0ld", "%.12lx",
    };
    static const long longValues[] = {0, 1, -1, 1234567890L, LONG_MAX, LONG_MIN};
    static const char* const longLongFormats[] = {
        "%lld", "%llu", "%llx", "%20lld", "%-20lld", "%.15lld", "%jd", "%ji", "%jx",
    };
    static const long long longLongValues[] = {0, 1, -1, 1234567890123456LL, LLONG_MAX, LLONG_MIN};
    static const char* const sizeFormats[] = {"%zu", "%zd", "%zx", "%tu", "%td"};
    static const long sizeValues[] = {0, 1, -1, 1234567890L, LONG_MAX, LONG_MIN};
    static const char* const stringFormats[] = {"%s", "%10s", "%-10s", "%.3s", "%.0s", "%10.3s",
                                                "%-10.3s", "%s%s", "a%sb"};
    static const char* const charFormats[] = {"%c", "%5c", "%-5c", "%c%c"};
    static const char* const pointerFormats[] = {"%p", "%20p", "%-20p"};
    static const char* const plainFormats[] = {"%5%", "%%", "plain text", "a%%b"};
    Barrage seen;
    CompareFormats(seen, signedFormats, sizeof signedFormats / sizeof *signedFormats, signedValues,
                   sizeof signedValues / sizeof *signedValues);
    CompareFormats(seen, unsignedFormats, sizeof unsignedFormats / sizeof *unsignedFormats,
                   unsignedValues, sizeof unsignedValues / sizeof *unsignedValues);
    CompareFormats(seen, longFormats, sizeof longFormats / sizeof *longFormats, longValues,
                   sizeof longValues / sizeof *longValues);
    CompareFormats(seen, longLongFormats, sizeof longLongFormats / sizeof *longLongFormats,
                   longLongValues, sizeof longLongValues / sizeof *longLongValues);
    CompareFormats(seen, sizeFormats, sizeof sizeFormats / sizeof *sizeFormats, sizeValues,
                   sizeof sizeValues / sizeof *sizeValues);
    for (const char* format : stringFormats) {
        CompareFormat(seen, format, "text", "more");
        CompareFormat(seen, format, "", "");
        CompareFormat(seen, format, (const char*)nullptr, (const char*)nullptr);
    }
    for (const char* format : charFormats) {
        CompareFormat(seen, format, 'A', 'b');
        CompareFormat(seen, format, 0, 65);
        CompareFormat(seen, format, 255, 1);
    }
    for (const char* format : pointerFormats) {
        CompareFormat(seen, format, (void*)0x1234);
        CompareFormat(seen, format, (void*)nullptr);
        CompareFormat(seen, format, (void*)-1);
    }
    for (const char* format : plainFormats) CompareFormat(seen, format);
    Expect(seen.clean, "snprintf matches glibc's bytes for the integer, string, pointer and %% formats");

    // Floats: zeros and negatives, tiny and huge exponents, rounding at the precision's edge, %g's
    // stripping, %a's exact form, and long double.
    static const char* const floatFormats[] = {
        "%f",    "%.0f",   "%.1f",   "%.2f",   "%.6f",   "%10.3f", "%-10.3f", "%010.3f",
        "%+.2f", "% .2f",  "%#.0f",  "%e",     "%.0e",   "%.3e",   "%.17e",   "%15.6e",
        "%G",    "%g",     "%.0g",   "%.1g",   "%.6g",   "%.17g",  "%#g",     "%#.3g",
        "%12.5g", "%-12.5g", "%a",   "%.0a",   "%.3a",   "%A",     "%20.4a",  "%.10g",
        "%.13a", "%.14a",  "%.20a", "%#a",    "%.1a",   "%.2a",
    };
    static const double floatValues[] = {
        0.0,
        -0.0,
        1.0,
        -1.0,
        0.5,
        1.5,
        2.5,
        1.0 / 3.0,
        0.1,
        0.05,
        123456789.123456789,
        1e-5,
        1e-300,
        1e300,
        DBL_MAX,
        DBL_MIN,
        5e-324,
        2.2250738585072014e-308,
        9007199254740993.0,
        1e6,
        0.000123456789,
        1e21,
        1e-21,
        1e100,
        -1e-100,
        0.0625,
        0.09375,
        15.99,
        0.99,
        3.0,
        16.0,
        1e-10,
    };
    Barrage floats;
    CompareFormats(floats, floatFormats, sizeof floatFormats / sizeof *floatFormats, floatValues,
                   sizeof floatValues / sizeof *floatValues);
    Expect(floats.clean, "snprintf matches glibc's bytes for %f, %e, %g and %a");

    // The non-finite values, and the long double forms.
    for (const char* format : {"%f", "%e", "%g", "%a", "%F", "%E", "%G", "%A"}) {
        CompareFormat(floats, format, (double)1.0 / 0.0);
        CompareFormat(floats, format, -(double)1.0 / 0.0);
        CompareFormat(floats, format, (double)0.0 / 0.0);
    }
    static const char* const longDoubleFormats[] = {"%Lf",  "%.3Lf", "%Le",   "%.15Le", "%Lg",
                                                    "%.21Lg", "%La", "%.0La", "%.20La", "%#La"};
    static const long double longDoubleValues[] = {0.0L, 1.5L, -2.25L, 1.0L / 3.0L, 1e4000L, 1e-4000L,
                                                   LDBL_MAX, LDBL_MIN};
    CompareFormats(floats, longDoubleFormats, sizeof longDoubleFormats / sizeof *longDoubleFormats,
                   longDoubleValues, sizeof longDoubleValues / sizeof *longDoubleValues);
    Expect(floats.clean, "snprintf matches glibc's bytes for infinities, NaNs and long double");

    // glibc's %#g loses the trailing zeros when rounding carries the value up a power of ten:
    // "%#.6g" of 999999.5 prints "1.e+06" where the standard asks for "1.00000e+06". cordinal prints
    // the standard form (nothing in the program formats with %#g).
    {
        char text[32] = {};
        snprintf(text, sizeof text, "%#.6g", 999999.5);
        Expect(!strcmp(text, "1.00000e+06"), "%#g keeps its trailing zeros when a carry adds a digit");
    }

    // The results a caller uses: truncation, a zero-length buffer, a null buffer, the counts %n
    // stores, and the growing and descriptor forms.
    {
        char mine[8] = "xxxxxxx", theirs[8] = "xxxxxxx";
        const int a = snprintf(mine, 5, "%s", "0123456789");
        const int b = GlibcPrintf(theirs, 5, "%s", "0123456789");
        Expect(a == 10 && a == b && !strcmp(mine, theirs), "snprintf truncates and still reports the whole length");
        const int none = snprintf(nullptr, 0, "%d", 1234);
        const int theirNone = GlibcPrintf(nullptr, 0, "%d", 1234);
        Expect(none == 4 && none == theirNone, "snprintf with no buffer counts the bytes");
    }
    {
        int mineAt = -1, theirAt = -1;
        const int a = snprintf(nullptr, 0, "abc%n", &mineAt);
        const int b = GlibcPrintf(nullptr, 0, "abc%n", &theirAt);
        Expect(a == b && mineAt == 3 && mineAt == theirAt, "%n counts the bytes written, as glibc's");
        short mineShort = -1, theirShort = -1;
        const int c = snprintf(nullptr, 0, "abcdef%hn", &mineShort);
        const int d = GlibcPrintf(nullptr, 0, "abcdef%hn", &theirShort);
        Expect(c == d && mineShort == 6 && mineShort == theirShort, "%hn stores a short count");
    }
    {
        char* mine = nullptr;
        char* theirs = nullptr;
        const int a = asprintf(&mine, "%s-%d-%.2f", "part", 7, 2.5);
        const int b = GlibcAsprintf(&theirs, "%s-%d-%.2f", "part", 7, 2.5);
        Expect(a == 11 && a == b && mine && theirs && !strcmp(mine, theirs), "asprintf matches glibc's text");
        free(mine);
        free(theirs);
    }
    {
        // A long result, past an internal buffer, and the whole thing arrives.
        std::string longText(5000, 'x');
        char* mine = nullptr;
        const int a = asprintf(&mine, "%s|%d", longText.c_str(), 9);
        Expect(a == 5002 && mine && strlen(mine) == 5002 && mine[5001] == '9',
               "asprintf grows past its first buffer");
        free(mine);
    }
    {
        const std::string path = dir + "/dprintf";
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int wrote = fd >= 0 ? dprintf(fd, "%s=%d", "k", 5) : -1;
        if (fd >= 0) close(fd);
        char readText[16] = {};
        const int readBack = open(path.c_str(), O_RDONLY);
        const long got = readBack >= 0 ? read(readBack, readText, sizeof readText - 1) : 0;
        if (readBack >= 0) close(readBack);
        unlink(path.c_str());
        Expect(wrote == 3 && got == 3 && !strcmp(readText, "k=5"),
               "dprintf writes the formatted bytes to a descriptor");
    }
    {
        const std::string path = dir + "/fprintf";
        FILE* file = fopen(path.c_str(), "w");
        const int wrote = file ? fprintf(file, "%5.2f|%-4d", 3.14159, 12) : -1;
        const int closed = file ? fclose(file) : -1;
        char text[32] = {};
        const int fd = open(path.c_str(), O_RDONLY);
        const long got = fd >= 0 ? read(fd, text, sizeof text - 1) : 0;
        if (fd >= 0) close(fd);
        unlink(path.c_str());
        Expect(wrote == 10 && closed == 0 && got == 10 && !strcmp(text, " 3.14|12  "),
               "fprintf writes through the stream and the file gets it at fclose");
    }
    {
        wchar_t mine[64] = {}, theirs[64] = {};
        static auto swprintf_ = Glibc<int (*)(wchar_t*, size_t, const wchar_t*, ...)>("swprintf");
        const int a = swprintf(mine, 64, L"%d|%s|%.2f|%ls|%lc", 5, "x", 1.25, L"wide", (wint_t)L'W');
        const int b = swprintf_
                          ? swprintf_(theirs, 64, L"%d|%s|%.2f|%ls|%lc", 5, "x", 1.25, L"wide", (wint_t)L'W')
                          : a;
        Expect(a == b && !wcscmp(mine, theirs), "swprintf matches glibc's wide text");
        wchar_t small[4] = {}, theirSmall[4] = {};
        const int c = swprintf(small, 4, L"%ls", L"abcdef");
        const int d = swprintf_ ? swprintf_(theirSmall, 4, L"%ls", L"abcdef") : c;
        Expect(c == -1 && c == d, "swprintf reports a buffer too small, as glibc's does");
    }
}

// One stream of each kind (cordinal's and glibc's) on the same file, taking the same calls and
// comparing every answer, so a difference names the call that made it.
struct StreamPair {
    FILE* mine = nullptr;
    FILE* theirs = nullptr;
    bool clean = true;
    const char* firstName = "";

    void Note(bool same, const char* what) {
        if (!same) {
            if (clean) firstName = what;
            clean = false;
        }
    }
    void Read(size_t n, const char* what) {
        char a[512] = {}, b[512] = {};
        if (n > sizeof a) n = sizeof a;
        const size_t gotA = fread(a, 1, n, mine);
        const size_t gotB = Gs().fread(b, 1, n, theirs);
        Note(gotA == gotB && !memcmp(a, b, gotA), what);
    }
    void Getc(int times, const char* what) {
        bool same = true;
        for (int i = 0; i < times && same; ++i) same = fgetc(mine) == Gs().fgetc(theirs);
        Note(same, what);
    }
    void Ungetc(const char* bytes, const char* what) {
        bool same = true;
        for (const char* p = bytes; *p; ++p)
            same = ungetc((unsigned char)*p, mine) == Gs().ungetc((unsigned char)*p, theirs) && same;
        Note(same, what);
    }
    void Seek(long offset, int whence, const char* what) {
        Note(fseek(mine, offset, whence) == Gs().fseek(theirs, offset, whence), what);
    }
    void Tell(const char* what) { Note(ftell(mine) == Gs().ftell(theirs), what); }
    void Flags(const char* what) {
        Note(feof(mine) == Gs().feof(theirs) && ferror(mine) == Gs().ferror(theirs), what);
    }
    void Flush(const char* what) { Note(fflush(mine) == Gs().fflush(theirs), what); }
    void Close() {
        if (mine) fclose(mine);
        if (theirs) Gs().fclose(theirs);
        mine = theirs = nullptr;
    }
};

inline void WritePattern(const std::string& path, size_t size, int seed) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    std::vector<char> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = (char)((i * 7 + (unsigned)seed) & 0xFF);
    size_t done = 0;
    while (done < size) {
        const long wrote = write(fd, bytes.data() + done, size - done);
        if (wrote <= 0) break;
        done += (size_t)wrote;
    }
    close(fd);
}

inline std::string ReadFile(const std::string& path) {
    std::string text;
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return text;
    char buffer[4096];
    for (long got; (got = read(fd, buffer, sizeof buffer)) > 0;) text.append(buffer, (size_t)got);
    close(fd);
    return text;
}

inline void CheckPositions(const std::string& dir) {
    const std::string path = dir + "/positions";
    WritePattern(path, 20000, 3);
    StreamPair stream;
    stream.mine = fopen(path.c_str(), "r");
    stream.theirs = Gs().fopen(path.c_str(), "r");
    if (!stream.mine || !stream.theirs) {
        Expect(false, "open the position file twice");
        stream.Close();
        return;
    }
    stream.Read(10, "the first ten bytes of a file");
    stream.Tell("ftell after a read");
    stream.Seek(5, SEEK_CUR, "fseek forward from the current place");
    stream.Tell("ftell after a relative seek");
    stream.Seek(-3, SEEK_CUR, "fseek back over the current place");
    stream.Getc(1, "getc after the seek");
    stream.Tell("ftell after getc");
    stream.Ungetc("AB", "two ungetc calls");
    stream.Tell("ftell with pushed-back bytes");
    stream.Getc(3, "the pushed-back bytes come back first, in reverse");
    stream.Tell("ftell after the pushback is used up");
    stream.Seek(0, SEEK_END, "fseek to the end");
    stream.Tell("ftell at the end");
    stream.Seek(8190, SEEK_SET, "fseek into the middle of a buffer");
    stream.Read(1, "a read across the buffer's edge");
    stream.Ungetc("xyz", "ungetc at a buffer boundary");
    stream.Getc(4, "getc across the buffer boundary after ungetc");
    stream.Tell("ftell after crossing the boundary");
    stream.Seek(0, SEEK_SET, "rewind to the start with fseek");
    stream.Read(4, "read after the rewind");
    stream.Flush("fflush on a read stream drops what was read ahead");
    stream.Tell("ftell after fflush on a read stream");
    stream.Read(4, "read after fflush on a read stream");
    Expect(stream.clean, "the stream answers every seek, tell, read and ungetc call as glibc's does");

    // End of file and the error flag, and ungetc at the end of the file.
    stream.Seek(0, SEEK_END, "seek to the end for the flags");
    stream.Read(16, "a read at the end returns nothing");
    stream.Flags("the end-of-file flag after reading nothing");
    stream.Read(16, "a second read at the end");
    stream.Getc(1, "getc at the end");
    stream.Flags("the flags after getc at the end");
    stream.Ungetc("Q", "ungetc at the end of the file");
    stream.Flags("ungetc clears the end-of-file flag");
    stream.Getc(2, "the pushed-back byte, then the end again");
    stream.Flags("the flags after the pushed-back byte");
    rewind(stream.mine);
    Gs().fseek(stream.theirs, 0, SEEK_SET);
    stream.Flags("the end-of-file flag after a rewind");
    stream.Read(4, "read after the rewind");
    stream.Close();
    Expect(stream.clean, "the end-of-file flag and ungetc at the end answer as glibc's do");

    // fgets, including the line that fills the buffer exactly and n == 1.
    stream.mine = fopen(path.c_str(), "r");
    stream.theirs = Gs().fopen(path.c_str(), "r");
    if (stream.mine && stream.theirs) {
        bool same = true;
        for (int n : {2, 5, 1, 4096}) {
            char a[4096] = {}, b[4096] = {};
            const char* gotA = fgets(a, n, stream.mine);
            const char* gotB = Gs().fgets(b, n, stream.theirs);
            if ((gotA == nullptr) != (gotB == nullptr) || (gotA && strcmp(a, b))) {
                if (same)
                    fprintf(stderr, "  fgets(%d): cordinal \"%s\", glibc \"%s\"\n", n, gotA ? a : "(null)",
                            gotB ? b : "(null)");
                same = false;
            }
        }
        Expect(same, "fgets matches glibc for short, empty and full lines");
        stream.Close();
    }
    unlink(path.c_str());
}

// The observable effect of a buffering mode: whether the bytes reach the pipe before a flush, and
// what a flush then delivers. `ours` picks the stream layer; glibc's is the same test on its own.
struct Buffering {
    bool beforeFlush = false;
    std::string afterFlush;
};

inline Buffering TryBuffering(bool ours, int mode) {
    Buffering result;
    int fds[2];
    if (pipe(fds) != 0) return result;
    FILE* stream = ours ? fdopen(fds[1], "w") : Gs().fdopen(fds[1], "w");
    if (!stream) {
        close(fds[0]);
        close(fds[1]);
        return result;
    }
    const int set = ours ? setvbuf(stream, nullptr, mode, 0) : Gs().setvbuf(stream, nullptr, mode, 0);
    if (ours)
        fputs("held", stream);
    else
        Gs().fputs("held", stream);
    pollfd ready{fds[0], POLLIN, 0};
    result.beforeFlush = set == 0 && poll(&ready, 1, 0) == 1;
    if (result.beforeFlush) {
        char early[16] = {};
        const long got = read(fds[0], early, sizeof early - 1);
        if (got > 0) result.afterFlush.append(early, (size_t)got);
    }
    if (ours) {
        fflush(stream);
        fclose(stream);
    } else {
        Gs().fflush(stream);
        Gs().fclose(stream);
    }
    pollfd again{fds[0], POLLIN, 0};
    if (poll(&again, 1, 100) == 1) {
        char late[16] = {};
        const long got = read(fds[0], late, sizeof late - 1);
        if (got > 0) result.afterFlush.append(late, (size_t)got);
    }
    close(fds[0]);
    return result;
}

inline void CheckBuffering() {
    struct Case {
        int mode;
        const char* name;
        bool reachesEarly;
    };
    const Case cases[] = {
        {_IONBF, "unbuffered", true}, {_IOLBF, "line buffered", false}, {_IOFBF, "fully buffered", false}};
    bool clean = true;
    for (const Case& c : cases) {
        const Buffering mine = TryBuffering(true, c.mode);
        const Buffering theirs = TryBuffering(false, c.mode);
        if (mine.beforeFlush != c.reachesEarly || theirs.beforeFlush != c.reachesEarly ||
            mine.beforeFlush != theirs.beforeFlush || mine.afterFlush != theirs.afterFlush) {
            if (clean)
                fprintf(stderr, "  %s: cordinal early %d \"%s\", glibc early %d \"%s\"\n", c.name,
                        (int)mine.beforeFlush, mine.afterFlush.c_str(), (int)theirs.beforeFlush,
                        theirs.afterFlush.c_str());
            clean = false;
        }
    }
    Expect(clean, "unbuffered, line-buffered and fully buffered streams reach the pipe as glibc's do");

    // Line buffering sends the line at its newline, and only then.
    int fds[2];
    if (pipe(fds) == 0) {
        FILE* line = fdopen(fds[1], "w");
        bool ok = line && setvbuf(line, nullptr, _IOLBF, 0) == 0;
        pollfd ready{fds[0], POLLIN, 0};
        ok = ok && fputs("abc", line) >= 0 && poll(&ready, 1, 0) == 0;
        ok = ok && fputc('\n', line) == '\n' && poll(&ready, 1, 100) == 1;
        char text[8] = {};
        const long got = read(fds[0], text, sizeof text - 1);
        ok = ok && got == 4 && !strcmp(text, "abc\n");
        if (line) fclose(line);
        close(fds[0]);
        Expect(ok, "a line-buffered stream to a pipe sends the line at its newline");
    }
    // A fully buffered stream holds its bytes until a flush.
    if (pipe(fds) == 0) {
        FILE* full = fdopen(fds[1], "w");
        bool ok = full && setvbuf(full, nullptr, _IOFBF, 0) == 0 && fputs("hold", full) >= 0;
        pollfd ready{fds[0], POLLIN, 0};
        ok = ok && poll(&ready, 1, 0) == 0 && fflush(full) == 0 && poll(&ready, 1, 100) == 1;
        char text[8] = {};
        const long got = read(fds[0], text, sizeof text - 1);
        ok = ok && got == 4 && !strcmp(text, "hold");
        if (full) fclose(full);
        close(fds[0]);
        Expect(ok, "a fully buffered stream holds its bytes until a flush");
    }
    // A caller's own buffer, and setbuf.
    {
        FILE* file = fopen("/dev/null", "w");
        static char buffer[64];
        const bool ok = file && setvbuf(file, buffer, _IOFBF, sizeof buffer) == 0 && fputs("x", file) >= 0;
        setbuf(file, nullptr);
        const bool wrote = ok && fputs("y", file) >= 0;
        const int closed = file ? fclose(file) : -1;
        Expect(wrote && closed == 0, "setvbuf takes a caller's buffer and setbuf turns buffering off");
    }
}

inline void CheckFlushes(const std::string& dir) {
    // fflush(NULL) sends every stream's pending bytes, and the file gets them at once.
    const std::string path = dir + "/fflush-all";
    FILE* file = fopen(path.c_str(), "w");
    if (file) {
        const bool wrote = fputs("before the flush", file) >= 0;
        const bool emptyBefore = ReadFile(path).empty();
        const int flushed = fflush(nullptr);
        const std::string after = ReadFile(path);
        Expect(wrote && emptyBefore && flushed == 0 && after == "before the flush",
               "fflush(NULL) writes every stream's pending bytes");
        fclose(file);
        unlink(path.c_str());
    } else {
        Expect(false, "open the fflush(NULL) file");
    }

    // One stream's flush leaves the others alone.
    {
        FILE* first = fopen((dir + "/flush-one").c_str(), "w");
        FILE* second = fopen((dir + "/flush-two").c_str(), "w");
        bool ok = first && second && fputs("one", first) >= 0 && fputs("two", second) >= 0;
        ok = ok && ReadFile((dir + "/flush-one").c_str()).empty() && fflush(first) == 0;
        ok = ok && ReadFile((dir + "/flush-one").c_str()) == "one" &&
             ReadFile((dir + "/flush-two").c_str()).empty();
        ok = ok && fflush(second) == 0 && ReadFile((dir + "/flush-two").c_str()) == "two";
        if (first) fclose(first);
        if (second) fclose(second);
        unlink((dir + "/flush-one").c_str());
        unlink((dir + "/flush-two").c_str());
        Expect(ok, "fflush writes one stream and leaves the others buffered");
    }

    // stdout itself: nothing arrives before the flush, and everything after it.
    fflush(stdout);  // the check runner's own output, so the pipe below sees only this test's bytes
    const int saved = dup(1);
    int fds[2];
    if (saved >= 0 && pipe(fds) == 0 && dup2(fds[1], 1) >= 0) {
        const bool wrote = fputs("to stdout", stdout) >= 0;
        pollfd ready{fds[0], POLLIN, 0};
        const bool held = poll(&ready, 1, 0) == 0;
        const int flushed = fflush(stdout);
        char text[32] = {};
        long got = 0;
        if (poll(&ready, 1, 100) == 1) got = read(fds[0], text, sizeof text - 1);
        dup2(saved, 1);
        close(saved);
        close(fds[0]);
        close(fds[1]);
        Expect(wrote && held && flushed == 0 && got == 9 && !strcmp(text, "to stdout"),
               "stdout holds its bytes until fflush and then delivers them");
    } else {
        if (saved >= 0) close(saved);
        Expect(false, "a pipe for the stdout flush check");
    }

    // The standard streams are the descriptors the program starts with, whatever else changed.
    Expect(fileno(stdin) == 0 && fileno(stdout) == 1 && fileno(stderr) == 2,
           "stdin, stdout and stderr hold descriptors 0, 1 and 2");

    // stderr is unbuffered: a child's byte is on the pipe before the child returns, so a buffered
    // stream would lose it (the child leaves through _exit, which flushes nothing).
    if (pipe(fds) == 0) {
        const pid_t child = fork();
        if (child == 0) {
            close(fds[0]);
            dup2(fds[1], 2);
            close(fds[1]);
            fputs("err", stderr);  // no flush
            usleep(300000);
            _exit(0);
        }
        close(fds[1]);
        pollfd ready{fds[0], POLLIN, 0};
        const bool early = poll(&ready, 1, 150) == 1;
        char text[16] = {};
        long got = early ? read(fds[0], text, sizeof text - 1) : 0;
        int status = 0;
        if (child > 0) waitpid(child, &status, 0);
        close(fds[0]);
        Expect(early && got == 3 && !strcmp(text, "err"), "stderr is unbuffered, so a byte is out at once");
    } else {
        Expect(false, "a pipe for the stderr check");
    }

    // The flush registered at exit: a child writes and returns through exit(), and the parent reads
    // the bytes from the pipe, so the atexit flush had to run.
    if (pipe(fds) == 0) {
        const pid_t child = fork();
        if (child == 0) {
            close(fds[0]);
            dup2(fds[1], 1);
            close(fds[1]);
            fputs("at exit", stdout);  // still buffered, and no explicit flush
            exit(0);
        }
        close(fds[1]);
        char text[32] = {};
        long got = 0;
        int status = 0;
        if (child > 0) {
            got = read(fds[0], text, sizeof text - 1);
            waitpid(child, &status, 0);
        }
        close(fds[0]);
        Expect(child > 0 && got == 7 && !strcmp(text, "at exit") && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0,
               "buffered output reaches its descriptor when the program returns from main");
    } else {
        Expect(false, "a pipe for the exit flush check");
    }
}

// Writes that fail, and reads and writes that only partly happen, answer as glibc's do.
inline void CheckFailures() {
    // /dev/full: the write itself fails with ENOSPC, unbuffered at once.
    {
        FILE* mine = fopen("/dev/full", "w");
        FILE* theirs = Gs().fopen("/dev/full", "w");
        if (mine && theirs) {
            setvbuf(mine, nullptr, _IONBF, 0);
            Gs().setvbuf(theirs, nullptr, _IONBF, 0);
            errno = 0;
            const size_t wroteA = fwrite("x", 1, 1, mine);
            const int errorA = errno;
            errno = 0;
            const size_t wroteB = Gs().fwrite("x", 1, 1, theirs);
            const int errorB = errno;
            const bool flags = ferror(mine) && Gs().ferror(theirs);
            fclose(mine);
            Gs().fclose(theirs);
            Expect(wroteA == wroteB && errorA == errorB && errorA == ENOSPC && flags,
                   "an unbuffered write to a full device fails with ENOSPC, as glibc's");
        } else {
            if (mine) fclose(mine);
            if (theirs) Gs().fclose(theirs);
            Expect(false, "open /dev/full twice");
        }
    }
    // Buffered, the failure waits for the flush.
    {
        FILE* mine = fopen("/dev/full", "w");
        FILE* theirs = Gs().fopen("/dev/full", "w");
        if (mine && theirs) {
            const size_t wroteA = fwrite("x", 1, 1, mine);
            Gs().fwrite("x", 1, 1, theirs);
            errno = 0;
            const int flushA = fflush(mine);
            const int errorA = errno;
            errno = 0;
            const int flushB = Gs().fflush(theirs);
            const int errorB = errno;
            const bool flags = ferror(mine) && Gs().ferror(theirs);
            const int closeA = fclose(mine);
            const int closeB = Gs().fclose(theirs);
            Expect(wroteA == 1 && flushA == flushB && errorA == errorB && errorA == ENOSPC && flags &&
                       closeA == closeB,
                   "a buffered write to a full device reports ENOSPC at the flush and the close, as glibc's");
        } else {
            if (mine) fclose(mine);
            if (theirs) Gs().fclose(theirs);
            Expect(false, "open /dev/full twice for the buffered check");
        }
    }

    // A pipe that is full and non-blocking takes the part that fits and reports the error.
    {
        int fds[2];
        if (pipe(fds) == 0) {
            const int flags = fcntl(fds[1], F_GETFL);
            fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);
            FILE* stream = fdopen(fds[1], "w");
            bool ok = stream && setvbuf(stream, nullptr, _IONBF, 0) == 0;
            std::vector<char> big(1 << 20, 'p');
            errno = 0;
            const size_t wrote = stream ? fwrite(big.data(), 1, big.size(), stream) : 0;
            ok = ok && errno == EAGAIN && wrote > 0 && wrote < big.size() && ferror(stream);
            if (stream) fclose(stream);
            std::vector<char> drained((size_t)wrote + 16);
            long got = 0;
            for (long n; (n = read(fds[0], drained.data() + got, drained.size() - (size_t)got)) > 0;)
                got += n;
            ok = ok && got == (long)wrote;
            close(fds[0]);
            Expect(ok, "a full non-blocking pipe takes part of the write, which is what fwrite reports");
        }
    }

    // A read from a pipe stops at what the writer sent and sets the end-of-file flag.
    {
        int fds[2];
        if (pipe(fds) == 0 && write(fds[1], "abc", 3) == 3) {
            close(fds[1]);
            FILE* stream = fdopen(fds[0], "r");
            char text[16] = {};
            const size_t got = stream ? fread(text, 1, sizeof text, stream) : 0;
            const bool flags = stream && feof(stream) && !ferror(stream);
            if (stream) fclose(stream);
            Expect(got == 3 && !strcmp(text, "abc") && flags,
                   "fread on a pipe returns what arrived and then the end-of-file flag");
        }
    }

    // Whole items: a short read reports only the items that came whole, as glibc's does.
    {
        int fds[2];
        if (pipe(fds) == 0 && write(fds[1], "abcdefg", 7) == 7) {
            close(fds[1]);
            FILE* stream = fdopen(fds[0], "r");
            char items[16] = {};
            const size_t got = stream ? fread(items, 3, 3, stream) : 0;
            if (stream) fclose(stream);
            Expect(got == 2, "fread reports whole items only");
        }
    }
}

inline void CheckNames(const std::string& dir) {
    // remove, rename and a stream reopened on another file, against glibc's answers.
    const std::string first = dir + "/name-one", second = dir + "/name-two";
    FILE* file = fopen(first.c_str(), "w");
    if (file) {
        fputs("bytes", file);
        fclose(file);
    }
    Expect(rename(first.c_str(), second.c_str()) == 0 && ReadFile(second) == "bytes", "rename moves a file");
    errno = 0;
    const int missing = remove((dir + "/absent").c_str());
    const int missingError = errno;
    errno = 0;
    const int theirMissing = Gs().remove((dir + "/absent").c_str());
    const int theirMissingError = errno;
    Expect(missing == theirMissing && missingError == theirMissingError && missingError == ENOENT,
           "remove of a missing name fails with ENOENT, as glibc's");
    Expect(remove(second.c_str()) == 0 && ReadFile(second).empty(), "remove deletes a file");
    const std::string folder = dir + "/name-folder";
    mkdir(folder.c_str(), 0700);
    Expect(remove(folder.c_str()) == 0, "remove of a folder removes it, as glibc's does");

    // freopen: the stream moves to another file and writes there.
    const std::string moved = dir + "/moved";
    FILE* stream = fopen(second.c_str(), "w");
    bool ok = stream != nullptr && freopen(moved.c_str(), "w", stream) == stream;
    ok = ok && fputs("moved", stream) >= 0 && fclose(stream) == 0;
    Expect(ok && ReadFile(moved) == "moved", "freopen moves a stream to another file");
    unlink(moved.c_str());
}

inline void CheckGetline(const std::string& dir) {
    const std::string path = dir + "/lines";
    {
        FILE* write = fopen(path.c_str(), "w");
        if (!write) {
            Expect(false, "open the getline file");
            return;
        }
        std::string longLine(5000, 'g');
        fputs("alpha;beta;gamma\n", write);
        fputs(longLine.c_str(), write);
        fputc('\n', write);
        fputc('\n', write);
        fputs("last", write);
        fclose(write);
    }
    FILE* mine = fopen(path.c_str(), "r");
    FILE* theirs = Gs().fopen(path.c_str(), "r");
    if (!mine || !theirs) {
        Expect(false, "open the getline file twice");
        if (mine) fclose(mine);
        if (theirs) Gs().fclose(theirs);
        return;
    }
    char* mineLine = nullptr;
    char* theirLine = nullptr;
    size_t mineCapacity = 0, theirCapacity = 0;
    bool same = true;
    long lengths[6] = {};
    int reads = 0;
    for (; reads < 6; ++reads) {
        const long a = getdelim(&mineLine, &mineCapacity, '\n', mine);
        const long b = Gs().getdelim(&theirLine, &theirCapacity, '\n', theirs);
        lengths[reads] = a;
        if (a != b || (a >= 0 && strcmp(mineLine, theirLine))) same = false;
        if (a < 0) break;
    }
    Expect(same && lengths[0] == 17 && lengths[1] == 5001 && lengths[2] == 1 && lengths[3] == 4 &&
               lengths[4] == -1 && mineCapacity > 5000,
           "getdelim matches glibc's lines, lengths and buffer growth");
    free(mineLine);
    free(theirLine);
    fclose(mine);
    Gs().fclose(theirs);

    // ';' as the delimiter, and getline's own name for a newline.
    mine = fopen(path.c_str(), "r");
    if (mine) {
        char* line = nullptr;
        size_t capacity = 0;
        const long first = getdelim(&line, &capacity, ';', mine);
        const long second = getdelim(&line, &capacity, ';', mine);
        const long third = getdelim(&line, &capacity, ';', mine);
        Expect(first == 6 && second == 5 && third > 5000 && line[0] == 'g',
               "getdelim stops at the delimiter asked for");
        (void)third;
        free(line);
        fclose(mine);
    }
    mine = fopen(path.c_str(), "r");
    if (mine) {
        char* line = nullptr;
        size_t capacity = 0;
        const long first = getline(&line, &capacity, mine);
        Expect(first == 17 && !strcmp(line, "alpha;beta;gamma\n"), "getline is getdelim on a newline");
        free(line);
        fclose(mine);
    }
    // At the end, getline reports -1 and leaves the end-of-file flag.
    mine = fopen((dir + "/empty").c_str(), "w");
    if (mine) fclose(mine);
    mine = fopen((dir + "/empty").c_str(), "r");
    if (mine) {
        char* line = nullptr;
        size_t capacity = 0;
        Expect(getline(&line, &capacity, mine) == -1 && feof(mine),
               "getline at the end of an empty file reports -1");
        free(line);
        fclose(mine);
    }
    unlink((dir + "/empty").c_str());
    unlink(path.c_str());
}

// One scanf conversion, both libraries taken through the same call with the same argument type: the
// return count, errno and the stored value must all match glibc's. A long double's slot holds six
// bytes of padding that a store need not write, so floating values compare by value.
template <class T>
inline bool SameScanValue(const T& mine, const T& theirs) {
    if constexpr (std::is_floating_point_v<T>) {
        if (mine == theirs) return true;
        return mine != mine && theirs != theirs;
    } else {
        return !memcmp(&mine, &theirs, sizeof mine);
    }
}

template <class T>
inline void CompareScanValue(Barrage& seen, const char* input, const char* format) {
    T mine{}, theirs{};
    errno = 0;
    const int mineCount = sscanf(input, format, &mine);
    const int mineError = errno;
    errno = 0;
    const int theirCount = GlibcScan(input, format, &theirs);
    const int theirError = errno;
    if (mineCount == theirCount && mineError == theirError && SameScanValue(mine, theirs)) return;
    uintmax_t bitsA = 0, bitsB = 0;
    const size_t width = sizeof mine < sizeof bitsA ? sizeof mine : sizeof bitsA;
    memcpy(&bitsA, &mine, width);
    memcpy(&bitsB, &theirs, width);
    char a[96], b[96];
    snprintf(a, sizeof a, "\"%s\": %d, errno %d, value %llx", input, mineCount, mineError,
             (unsigned long long)bitsA);
    snprintf(b, sizeof b, "\"%s\": %d, errno %d, value %llx", input, theirCount, theirError,
             (unsigned long long)bitsB);
    char what[256];
    snprintf(what, sizeof what, "\"%s\" with \"%s\"", input, format);
    seen.Differ(what, a, b);
}

// One string conversion, comparing the whole destination buffer so %s's terminator and %c's lack of
// one are both covered.
inline void CompareScanText(Barrage& seen, const char* input, const char* format) {
    char mine[64], theirs[64];
    memset(mine, 0xA5, sizeof mine);
    memset(theirs, 0xA5, sizeof theirs);
    mine[sizeof mine - 1] = theirs[sizeof theirs - 1] = 0;
    errno = 0;
    const int mineCount = sscanf(input, format, mine);
    const int mineError = errno;
    errno = 0;
    const int theirCount = GlibcScan(input, format, theirs);
    const int theirError = errno;
    if (mineCount == theirCount && mineError == theirError && !memcmp(mine, theirs, sizeof mine)) return;
    char a[128], b[128];
    snprintf(a, sizeof a, "%d, errno %d, \"%s\"", mineCount, mineError, mine);
    snprintf(b, sizeof b, "%d, errno %d, \"%s\"", theirCount, theirError, theirs);
    char what[256];
    snprintf(what, sizeof what, "\"%s\" with \"%s\"", input, format);
    seen.Differ(what, a, b);
}

inline void CheckScanf() {
    Barrage seen;

    // Integers: bases and prefixes (C23's "0b" included), signs, widths, every length modifier,
    // overflow, and what a failed match or a run out of input returns.
    CompareScanValue<int>(seen, "42x", "%d");
    CompareScanValue<int>(seen, "  -42", "%d");
    CompareScanValue<int>(seen, "+17", "%d");
    CompareScanValue<int>(seen, "-0x10", "%i");
    CompareScanValue<int>(seen, "0x1f", "%i");
    CompareScanValue<int>(seen, "0b101", "%i");
    CompareScanValue<int>(seen, "017", "%i");
    CompareScanValue<int>(seen, "0x", "%i");
    CompareScanValue<int>(seen, "0b", "%i");
    CompareScanValue<int>(seen, "08", "%i");
    CompareScanValue<int>(seen, "0b101", "%d");
    CompareScanValue<int>(seen, "123456789", "%2d");
    CompareScanValue<int>(seen, "-7", "%1d");
    CompareScanValue<int>(seen, "999999999999999999999", "%d");
    CompareScanValue<int>(seen, "-999999999999999999999", "%d");
    CompareScanValue<unsigned>(seen, "4294967295", "%u");
    CompareScanValue<unsigned>(seen, "-17", "%u");
    CompareScanValue<unsigned>(seen, "0xffffffff", "%x");
    CompareScanValue<unsigned>(seen, "0XABCDEF", "%X");
    CompareScanValue<unsigned>(seen, "777", "%o");
    CompareScanValue<unsigned>(seen, "+", "%u");
    CompareScanValue<void*>(seen, "0x1234", "%p");
    CompareScanValue<signed char>(seen, "127", "%hhd");
    CompareScanValue<signed char>(seen, "-128", "%hhd");
    CompareScanValue<short>(seen, "40000", "%hd");
    CompareScanValue<long>(seen, "-123456789012", "%ld");
    CompareScanValue<long long>(seen, "123456789012345678", "%lld");
    CompareScanValue<unsigned long long>(seen, "18446744073709551615", "%llu");
    CompareScanValue<size_t>(seen, "12345", "%zu");
    CompareScanValue<intmax_t>(seen, "9223372036854775807", "%jd");
    CompareScanValue<ptrdiff_t>(seen, "-9", "%td");
    CompareScanValue<int>(seen, "", "%d");
    CompareScanValue<int>(seen, "   ", "%d");
    CompareScanValue<int>(seen, "abc", "%d");

    // Strings, characters and character classes: widths, the scannable sets, inversion, a leading
    // ']' or '-', a range, and suppression.
    CompareScanText(seen, "hello world", "%s");
    CompareScanText(seen, "  hello", "%5s");
    CompareScanText(seen, "abc", "%10s");
    CompareScanText(seen, "abcdef", "%3c");
    CompareScanText(seen, "ab", "%3c");
    CompareScanText(seen, "abcdef", "%c");
    CompareScanText(seen, "]ab", "%[]a-z]");
    CompareScanText(seen, "-ab", "%[-a]");
    CompareScanText(seen, "cab", "%[^a]");
    CompareScanText(seen, "00123", "%[0-1]");
    CompareScanText(seen, "12ab34", "%[0-9]");
    CompareScanText(seen, "xyz", "%[abc]");
    CompareScanText(seen, "12a34b", "%[^ab]");
    CompareScanText(seen, "abc123", "%[a-z]");

    // %n and the return counts, in the places glibc counts them.
    {
        int mineValue = 0, theirValue = 0;
        int mineAt = -1, theirAt = -1;
        const int mineCount = sscanf("12 34", "%d %n", &mineValue, &mineAt);
        const int theirCount = GlibcScan("12 34", "%d %n", &theirValue, &theirAt);
        Expect(mineCount == theirCount && mineValue == theirValue && mineAt == theirAt && mineAt == 3,
               "%n reports the characters used, as glibc's");
        mineAt = theirAt = -1;
        sscanf("  12", " %n", &mineAt);
        GlibcScan("  12", " %n", &theirAt);
        Expect(mineAt == theirAt && mineAt == 2, "%n after whitespace counts the skipped spaces");
        int mineSecond = 0, theirSecond = 0;
        const int mineStopped = sscanf("12x 34", "%d %d", &mineValue, &mineSecond);
        const int theirStopped = GlibcScan("12x 34", "%d %d", &theirValue, &theirSecond);
        Expect(mineStopped == theirStopped && mineSecond == theirSecond && mineStopped == 1,
               "a failed second conversion returns the assignments made so far");
        Expect(sscanf("", "%d", &mineValue) == -1 && GlibcScan("", "%d", &theirValue) == -1,
               "running out of input before any assignment reports EOF");
        Expect(sscanf("abc", "%d", &mineValue) == 0 && GlibcScan("abc", "%d", &theirValue) == 0,
               "a failed first conversion with input left reports 0");
        int suppressed = 0;
        Expect(sscanf("7 8", "%*d %d", &suppressed) == 1 && suppressed == 8,
               "a suppressed conversion stores nothing and does not count");
    }

    // Literal characters, the whitespace directive and %% against glibc's own answers.
    {
        int mineValue = -1, theirValue = -1;
        int mineCount = sscanf("ab 12", "ab %d", &mineValue);
        int theirCount = GlibcScan("ab 12", "ab %d", &theirValue);
        Expect(mineCount == theirCount && mineValue == theirValue && mineValue == 12,
               "a literal prefix and a whitespace directive match glibc");
        mineCount = sscanf("%", "%%");
        theirCount = GlibcScan("%", "%%");
        Expect(mineCount == theirCount, "%% matches a literal percent as glibc's does");
        mineCount = sscanf(" %", "%%");
        theirCount = GlibcScan(" %", "%%");
        Expect(mineCount == theirCount, "a literal percent behind whitespace answers as glibc's");
        mineCount = sscanf("x", "y");
        theirCount = GlibcScan("x", "y");
        Expect(mineCount == theirCount, "a mismatched literal answers as glibc's");
        mineCount = sscanf("", "y");
        theirCount = GlibcScan("", "y");
        Expect(mineCount == theirCount, "a literal at the end of the input answers as glibc's");
    }

    // Floats: the grammar glibc accepts, its answers for the edges, and the length modifiers.
    CompareScanValue<float>(seen, "1.5e3x", "%f");
    CompareScanValue<double>(seen, "1.5e3x", "%lf");
    CompareScanValue<long double>(seen, "1.5e3x", "%Lf");
    CompareScanValue<double>(seen, "1e", "%lf");
    CompareScanValue<double>(seen, "1e+", "%lf");
    CompareScanValue<double>(seen, "1.5e+x", "%lf");
    CompareScanValue<double>(seen, "1.5e3x", "%lf");
    CompareScanValue<double>(seen, "1.5x", "%lf");
    CompareScanValue<double>(seen, "0x1p3", "%lf");
    CompareScanValue<double>(seen, "0x", "%lf");
    CompareScanValue<double>(seen, "0xg", "%lf");
    CompareScanValue<double>(seen, "infinity", "%lf");
    CompareScanValue<double>(seen, "nan(chars)", "%lf");
    CompareScanValue<double>(seen, "-inf", "%lf");
    CompareScanValue<double>(seen, ".e3", "%lf");
    CompareScanValue<double>(seen, ".5e2z", "%lf");
    CompareScanValue<double>(seen, "1..5", "%lf");
    CompareScanValue<double>(seen, "1.2.3", "%lf");
    CompareScanValue<double>(seen, "1e310", "%lf");
    CompareScanValue<double>(seen, "1e-400", "%lf");
    CompareScanValue<float>(seen, "3.25", "%e");
    CompareScanValue<double>(seen, "6.5", "%lg");

    Expect(seen.clean, "scanf matches glibc's answers for integers, strings, classes, %n and floats");

    // Wide input: the same scans from a wide string, and the wide destination forms.
    {
        Barrage wide;
        int mineValue = 0, theirValue = 0;
        wchar_t mineText[32] = {}, theirText[32] = {};
        const int mineCount = swscanf(L"12 hello", L"%d %ls", &mineValue, mineText);
        const int theirCount = GlibcWideScan(L"12 hello", L"%d %ls", &theirValue, theirText);
        if (mineCount != theirCount || mineValue != theirValue || wcscmp(mineText, theirText)) {
            char a[64], b[64];
            snprintf(a, sizeof a, "%d %d", mineCount, mineValue);
            snprintf(b, sizeof b, "%d %d", theirCount, theirValue);
            wide.Differ("wide %d %ls", a, b);
        }
        wchar_t mineChar = 0, theirChar = 0;
        const int mineOne = swscanf(L"Z", L"%lc", &mineChar);
        const int theirOne = GlibcWideScan(L"Z", L"%lc", &theirChar);
        if (mineOne != theirOne || mineChar != theirChar) wide.Differ("wide %lc", "differ", "differ");
        Expect(wide.clean, "swscanf matches glibc's wide scans");
    }

    // A narrow scan into wide destinations, and a wide scan into narrow ones: both are the locale's
    // conversion, which is glibc's until stage 5b.
    {
        wchar_t mineText[16] = {}, theirText[16] = {};
        const int mineCount = sscanf("ascii", "%ls", mineText);
        const int theirCount = GlibcScan("ascii", "%ls", theirText);
        Expect(mineCount == theirCount && !wcscmp(mineText, theirText),
               "a narrow scan into a wide string matches glibc's");
        char mineText2[16] = {}, theirText2[16] = {};
        const int mineCount2 = swscanf(L"ascii", L"%s", mineText2);
        const int theirCount2 = GlibcWideScan(L"ascii", L"%s", theirText2);
        Expect(mineCount2 == theirCount2 && !strcmp(mineText2, theirText2),
               "a wide scan into a narrow string matches glibc's");
    }
}

inline void CheckStdio(const std::string& root) {
    Expect(InProgram((const void*)&fopen) && InProgram((const void*)&fclose) && InProgram((const void*)&fwrite) &&
               InProgram((const void*)&fread) && InProgram((const void*)&fdopen) && InProgram((const void*)&printf) &&
               InProgram((const void*)&snprintf) && InProgram((const void*)&sscanf) &&
               InProgram((const void*)&fflush),
           "fopen, fdopen, fread, fwrite, printf, snprintf, sscanf and fflush are cordinal's own");
    const std::string dir = root + "/stdio";
    mkdir(dir.c_str(), 0700);
    CheckPrintf(dir);
    CheckPositions(dir);
    CheckBuffering();
    CheckFlushes(dir);
    CheckFailures();
    CheckNames(dir);
    CheckGetline(dir);
    CheckScanf();
    // Anything the checks left behind, so the folder can go.
    for (const char* name : {"/positions", "/lines", "/name-one", "/name-two", "/moved", "/absent",
                             "/dprintf", "/fprintf", "/fflush-all", "/flush-one", "/flush-two"})
        unlink((dir + name).c_str());
    rmdir(dir.c_str());
}

}  // namespace libc_checks
