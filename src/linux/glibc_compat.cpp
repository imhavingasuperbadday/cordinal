// Lets the program run on older glibc than the one it was built on (the release is built on
// Debian 13, glibc 2.41; Debian 12 has 2.36, Ubuntu 24.04 2.39). Built with _GNU_SOURCE (g++
// always defines it), glibc 2.38's headers send strtol, sscanf and friends to C23 versions
// (__isoc23_*, which also read "0b" binary prefixes), and newer libm adds new versions of fmod and
// hypotf. Defining the C23 names here, on top of glibc's older versions, and wrapping fmod and
// hypotf (-Wl,--wrap, see CMakeLists.txt) leaves glibc 2.36's arc4random (std::random_device, in the
// static libstdc++) as the newest symbol the program needs: Debian 12 and Ubuntu 24.04 run it.
// The one difference: strtol and its family don't read "0b" prefixes, as before 2.38 (cordinal never
// asks them to). Only for x86-64 glibc, where those old versions exist under these names.
#include <cstddef>  // for __GLIBC__
#if defined(__x86_64__) && defined(__GLIBC__) && defined(CORDINAL_GLIBC_COMPAT)
#include <cstdarg>
#include <cstddef>
#include <cwchar>

#define OLD(name, version) __asm__(".symver cordinal_old_" #name "," #name "@" version)
OLD(strtol, "GLIBC_2.2.5");
OLD(strtoll, "GLIBC_2.2.5");
OLD(strtoul, "GLIBC_2.2.5");
OLD(strtoull, "GLIBC_2.2.5");
OLD(wcstol, "GLIBC_2.2.5");
OLD(wcstoll, "GLIBC_2.2.5");
OLD(wcstoul, "GLIBC_2.2.5");
OLD(wcstoull, "GLIBC_2.2.5");
OLD(__isoc99_vsscanf, "GLIBC_2.7");
OLD(__isoc99_vswscanf, "GLIBC_2.7");
OLD(fmod, "GLIBC_2.2.5");
OLD(hypotf, "GLIBC_2.2.5");
#undef OLD

extern "C" {
long cordinal_old_strtol(const char*, char**, int);
long long cordinal_old_strtoll(const char*, char**, int);
unsigned long cordinal_old_strtoul(const char*, char**, int);
unsigned long long cordinal_old_strtoull(const char*, char**, int);
long cordinal_old_wcstol(const wchar_t*, wchar_t**, int);
long long cordinal_old_wcstoll(const wchar_t*, wchar_t**, int);
unsigned long cordinal_old_wcstoul(const wchar_t*, wchar_t**, int);
unsigned long long cordinal_old_wcstoull(const wchar_t*, wchar_t**, int);
int cordinal_old___isoc99_vsscanf(const char*, const char*, va_list);
int cordinal_old___isoc99_vswscanf(const wchar_t*, const wchar_t*, va_list);
double cordinal_old_fmod(double, double);
float cordinal_old_hypotf(float, float);

long __isoc23_strtol(const char* s, char** end, int base) { return cordinal_old_strtol(s, end, base); }
long long __isoc23_strtoll(const char* s, char** end, int base) { return cordinal_old_strtoll(s, end, base); }
unsigned long __isoc23_strtoul(const char* s, char** end, int base) { return cordinal_old_strtoul(s, end, base); }
unsigned long long __isoc23_strtoull(const char* s, char** end, int base) { return cordinal_old_strtoull(s, end, base); }
long __isoc23_wcstol(const wchar_t* s, wchar_t** end, int base) { return cordinal_old_wcstol(s, end, base); }
long long __isoc23_wcstoll(const wchar_t* s, wchar_t** end, int base) { return cordinal_old_wcstoll(s, end, base); }
unsigned long __isoc23_wcstoul(const wchar_t* s, wchar_t** end, int base) { return cordinal_old_wcstoul(s, end, base); }
unsigned long long __isoc23_wcstoull(const wchar_t* s, wchar_t** end, int base) { return cordinal_old_wcstoull(s, end, base); }
int __isoc23_sscanf(const char* s, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int n = cordinal_old___isoc99_vsscanf(s, format, ap);
    va_end(ap);
    return n;
}
int __isoc23_vsscanf(const char* s, const char* format, va_list ap) { return cordinal_old___isoc99_vsscanf(s, format, ap); }
int __isoc23_swscanf(const wchar_t* s, const wchar_t* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int n = cordinal_old___isoc99_vswscanf(s, format, ap);
    va_end(ap);
    return n;
}
double __wrap_fmod(double x, double y) { return cordinal_old_fmod(x, y); }
float __wrap_hypotf(float x, float y) { return cordinal_old_hypotf(x, y); }
}
#endif
