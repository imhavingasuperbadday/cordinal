// cordinal's clocks and calendar (time.cpp) against glibc's, for checks.h: every clock, gmtime_r
// and timegm over thousands of years with fields out of range, and localtime_r and mktime in
// zones with unusual rules (half-hour and 45-minute offsets, negative daylight time, Lord Howe's
// half-hour shift) and POSIX TZ strings, hour by hour across years and at the edges of every
// change. TimeBench() times them.
#pragma once
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

extern "C" __attribute__((visibility("hidden"))) int cordinal_time_vdso(void);

namespace libc_checks {

inline bool SameTm(const tm& a, const tm& b) {
    return a.tm_sec == b.tm_sec && a.tm_min == b.tm_min && a.tm_hour == b.tm_hour && a.tm_mday == b.tm_mday &&
           a.tm_mon == b.tm_mon && a.tm_year == b.tm_year && a.tm_wday == b.tm_wday && a.tm_yday == b.tm_yday &&
           a.tm_isdst == b.tm_isdst && a.tm_gmtoff == b.tm_gmtoff &&
           !strcmp(a.tm_zone ? a.tm_zone : "", b.tm_zone ? b.tm_zone : "");
}
inline void PrintTm(const char* who, const tm& t) {
    fprintf(stderr, "  %s: %d-%02d-%02d %02d:%02d:%02d wday %d yday %d isdst %d gmtoff %ld zone %s\n", who,
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday, t.tm_yday,
            t.tm_isdst, t.tm_gmtoff, t.tm_zone ? t.tm_zone : "(null)");
}

inline void CheckTime() {
    auto gClock = (int (*)(clockid_t, timespec*))dlsym(RTLD_DEFAULT, "clock_gettime");
    auto gTimeOfDay = (int (*)(timeval*, void*))dlsym(RTLD_DEFAULT, "gettimeofday");
    auto gGmtime = (tm * (*)(const time_t*, tm*)) dlsym(RTLD_DEFAULT, "gmtime_r");
    auto gTimegm = (time_t(*)(tm*))dlsym(RTLD_DEFAULT, "timegm");
    auto gLocaltime = (tm * (*)(const time_t*, tm*)) dlsym(RTLD_DEFAULT, "localtime_r");
    auto gMktime = (time_t(*)(tm*))dlsym(RTLD_DEFAULT, "mktime");
    auto gTzset = (void (*)())dlsym(RTLD_DEFAULT, "tzset");
    if (!gClock || !gTimeOfDay || !gGmtime || !gTimegm || !gLocaltime || !gMktime || !gTzset) {
        Expect(false, "glibc's time functions are found");
        return;
    }
    Expect(InProgram((const void*)&clock_gettime) && InProgram((const void*)&localtime_r) && !InProgram((const void*)gClock),
           "clock_gettime and localtime_r are cordinal's");
    Expect(cordinal_time_vdso() == 7, "the vDSO's clock_gettime, gettimeofday and time are found");

    // Clocks: each of ours read between two of glibc's (coarse clocks within their tick).
    struct {
        clockid_t id;
        long slack;
    } clocks[] = {{CLOCK_REALTIME, 0},         {CLOCK_MONOTONIC, 0},         {CLOCK_BOOTTIME, 0},
                  {CLOCK_MONOTONIC_RAW, 0},    {CLOCK_REALTIME_COARSE, 20000000}, {CLOCK_MONOTONIC_COARSE, 20000000},
                  {CLOCK_PROCESS_CPUTIME_ID, 0}, {CLOCK_THREAD_CPUTIME_ID, 0}};
    for (const auto& c : clocks) {
        timespec a{}, b{}, x{};
        const bool read = gClock(c.id, &a) == 0 && clock_gettime(c.id, &x) == 0 && gClock(c.id, &b) == 0;
        const int64_t na = a.tv_sec * 1000000000ll + a.tv_nsec, nb = b.tv_sec * 1000000000ll + b.tv_nsec;
        const int64_t nx = x.tv_sec * 1000000000ll + x.tv_nsec;
        if (!read || nx < na - c.slack || nx > nb + c.slack) {
            fprintf(stderr, "FAIL clock %d: %lld not between %lld and %lld\n", (int)c.id, (long long)nx, (long long)na, (long long)nb);
            ++failures;
        }
    }
    timespec ts{};
    errno = 0;
    Expect(clock_gettime((clockid_t)1000, &ts) == -1 && errno == EINVAL, "an unknown clock fails with EINVAL");
    timeval tv{}, gtv{};
    gTimeOfDay(&gtv, nullptr);
    Expect(gettimeofday(&tv, nullptr) == 0 && tv.tv_sec - gtv.tv_sec <= 1 && tv.tv_sec >= gtv.tv_sec, "gettimeofday");
    time_t now = 0;
    Expect(time(&now) == now && now >= gtv.tv_sec && now - gtv.tv_sec <= 1, "time");
    Expect(difftime(10, 3) == 7.0 && difftime(-5, 5) == -10.0, "difftime");

    Random r{0x5eed};
    int bad = 0;
    // The first differences of each kind in each zone, so one zone's problem doesn't hide another's.
    int shownHere = 0;
    auto Fail = [&](const char* what, long long t, const tm* ours, const tm* theirs) {
        ++bad;
        if (shownHere++ < 3) {
            fprintf(stderr, "FAIL %s at %lld (TZ=%s)\n", what, t, getenv("TZ") ? getenv("TZ") : "(unset)");
            if (ours) PrintTm("cordinal", *ours);
            if (theirs) PrintTm("glibc   ", *theirs);
        }
    };

    // gmtime_r and timegm, over thousands of years and with fields out of range.
    for (int i = 0; i < 200000; ++i) {
        const time_t t = (time_t)((int64_t)(r.Next() % 400000000000ull) - 200000000000ll);
        tm a{}, b{};
        const bool okA = gmtime_r(&t, &a) != nullptr, okB = gGmtime(&t, &b) != nullptr;
        if (okA != okB || (okA && !SameTm(a, b))) Fail("gmtime_r", t, &a, &b);
        tm in{};
        in.tm_year = (int)(r.Next() % 400) - 50;
        in.tm_mon = (int)(r.Next() % 61) - 30;
        in.tm_mday = (int)(r.Next() % 201) - 100;
        in.tm_hour = (int)(r.Next() % 101) - 50;
        in.tm_min = (int)(r.Next() % 2001) - 1000;
        in.tm_sec = (int)(r.Next() % 20001) - 10000;
        tm ca = in, cb = in;
        const time_t ta = timegm(&ca), tb = gTimegm(&cb);
        if (ta != tb || !SameTm(ca, cb)) Fail("timegm", (long long)ta, &ca, &cb);
    }
    for (time_t t : {(time_t)INT64_MAX, (time_t)INT64_MIN, (time_t)67768036191676799ll, (time_t)67768036191676800ll}) {
        tm a{}, b{};
        const bool okA = gmtime_r(&t, &a) != nullptr, okB = gGmtime(&t, &b) != nullptr;
        if (okA != okB || (okA && !SameTm(a, b))) Fail("gmtime_r at the limits", (long long)t, &a, &b);
    }

    // Local time in many zones. Zone files are skipped where the system has none.
    const char* zones[] = {nullptr, "", "UTC", "America/New_York", "Europe/London", "Europe/Dublin", "Europe/Berlin",
                           "Australia/Lord_Howe", "Australia/Sydney", "Asia/Kolkata", "Asia/Kathmandu", "Pacific/Chatham",
                           "America/Sao_Paulo", "Africa/Casablanca", "America/St_Johns", "America/Godthab",
                           "Antarctica/Troll", ":Asia/Tokyo", "/usr/share/zoneinfo/Pacific/Apia",
                           "EST5EDT,M3.2.0,M11.1.0", "<+0330>-3:30", "NZST-12NZDT,M9.5.0,M4.1.0/3",
                           "AEST-10AEDT,M10.1.0,M4.1.0/3", "XYZ3", "WET0WEST,M3.5.0/1,M10.5.0",
                           "IST-1GMT0,M10.5.0,M3.5.0/1", "<-02>2<-01>,M3.5.0/-1,M10.5.0/0", "JST-9",
                           "Nonsense/Zone", "EST5EDT4,J60/1:30,300/25"};
    int checkedZones = 0;
    for (const char* zone : zones) {
        if (zone && zone[0] && zone[0] != '<' && strchr(zone, '/') && !strchr(zone, ',') && strcmp(zone, "Nonsense/Zone")) {
            char path[256];
            snprintf(path, sizeof path, "%s%s", zone[0] == '/' ? "" : "/usr/share/zoneinfo/", zone[0] == ':' ? zone + 1 : zone);
            struct stat info;
            if (stat(path, &info)) continue;
        }
        if (zone) setenv("TZ", zone, 1);
        else unsetenv("TZ");
        tzset();
        gTzset();
        ++checkedZones;
        shownHere = 0;
        auto Compare = [&](time_t t) {
            tm a{}, b{};
            const bool okA = localtime_r(&t, &a) != nullptr, okB = gLocaltime(&t, &b) != nullptr;
            if (okA != okB || (okA && !SameTm(a, b))) Fail("localtime_r", (long long)t, &a, &b);
            return b;
        };
        // Every hour from 1965 to 1975 and from 2018 to 2045 (one of each other year), and random times.
        for (time_t t = -157766400; t < 157766400; t += 3600) Compare(t);
        for (time_t t = 1514764800; t < 2366755200; t += 3600 * ((t < 1609459200) ? 1 : 23)) Compare(t);
        for (int i = 0; i < 30000; ++i) Compare((time_t)((int64_t)(r.Next() % 20000000000ull) - 10000000000ll));
        // mktime on dates around each change found (gaps, repeats), with each tm_isdst hint, and on
        // fields out of range.
        for (time_t t = 1514764800; t < 1704067200; t += 3600) {
            tm before{}, after{};
            gLocaltime(&t, &before);
            const time_t next = t + 3600;
            gLocaltime(&next, &after);
            const bool change = before.tm_gmtoff != after.tm_gmtoff || before.tm_isdst != after.tm_isdst;
            if (!change && t % 86400 != 0) continue;  // ordinary hours once a day; every change in full
            for (int k = 0; k < (change ? 40 : 1); ++k) {
                tm base = before;
                base.tm_min = (int)(r.Next() % 60);
                base.tm_hour += (int)(r.Next() % 5) - 2;
                if (!change) base.tm_mday += (int)(r.Next() % 3) - 1;
                for (int hint : {-1, 0, 1}) {
                    tm ca = base, cb = base;
                    ca.tm_isdst = cb.tm_isdst = hint;
                    const time_t ta = mktime(&ca), tb = gMktime(&cb);
                    if (ta != tb || !SameTm(ca, cb)) {
                        Fail(hint < 0 ? "mktime (isdst -1)" : hint ? "mktime (isdst 1)" : "mktime (isdst 0)", (long long)tb, &ca, &cb);
                        if (shownHere <= 3) PrintTm("asked   ", base);
                    }
                }
            }
        }
    }
    unsetenv("TZ");
    tzset();
    gTzset();
    Expect(checkedZones >= 12, "time zones checked (the system needs its zone files for most)");
    if (checkedZones < 20) printf("note: %d of %zu zones checked; the others' files are missing\n", checkedZones, sizeof zones / sizeof *zones);
    if (bad) fprintf(stderr, "%d differences from glibc in all\n", bad);
    Expect(bad == 0, "clocks, calendar and local time match glibc's");
}

inline int TimeBench() {
    auto gClock = (int (*)(clockid_t, timespec*))dlsym(RTLD_DEFAULT, "clock_gettime");
    auto gLocaltime = (tm * (*)(const time_t*, tm*)) dlsym(RTLD_DEFAULT, "localtime_r");
    auto gMktime = (time_t(*)(tm*))dlsym(RTLD_DEFAULT, "mktime");
    auto gGmtime = (tm * (*)(const time_t*, tm*)) dlsym(RTLD_DEFAULT, "gmtime_r");
    int (*volatile ourClock)(clockid_t, timespec*) = &clock_gettime;
    tm* (*volatile ourLocal)(const time_t*, tm*) = &localtime_r;
    tm* (*volatile ourGm)(const time_t*, tm*) = &gmtime_r;
    time_t (*volatile ourMktime)(tm*) = &mktime;
    printf("%-26s %10s %10s %7s\n", "", "cordinal", "glibc", "ratio");
    auto Row = [&](const char* name, auto ours, auto theirs) {
        double a, b;
        TimeBoth(ours, theirs, a, b);
        printf("%-26s %8.1fns %8.1fns %6.2fx\n", name, a, b, a / b);
    };
    timespec ts{};
    Row("clock_gettime monotonic", [&] { ourClock(CLOCK_MONOTONIC, &ts); }, [&] { gClock(CLOCK_MONOTONIC, &ts); });
    Row("clock_gettime realtime", [&] { ourClock(CLOCK_REALTIME, &ts); }, [&] { gClock(CLOCK_REALTIME, &ts); });
    time_t t = 1700000000;
    tm out{};
    Row("gmtime_r", [&] { ourGm(&t, &out); }, [&] { gGmtime(&t, &out); });
    Row("localtime_r", [&] { ourLocal(&t, &out); }, [&] { gLocaltime(&t, &out); });
    tm in{};
    gLocaltime(&t, &in);
    Row("mktime", [&] { tm c = in; ourMktime(&c); }, [&] { tm c = in; gMktime(&c); });
    return 0;
}

}  // namespace libc_checks
