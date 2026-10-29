// Optional decoder-stage instrumentation for development probes only. Shipping
// builds contain neither counters nor timer calls.
#pragma once
#if defined(CORDINAL_AUDIO_BENCHMARK)
#include <windows.h>
#include <cstdint>
namespace nativeaudio::benchmark {
// Accumulated time (QPC ticks) and number of instrumented core calls.
inline uint64_t ticks=0,calls=0;
inline void Reset() { ticks=calls=0; }
class Scope {
    LARGE_INTEGER start_{};
public:
    Scope() { QueryPerformanceCounter(&start_); }
    ~Scope() { LARGE_INTEGER end;QueryPerformanceCounter(&end);ticks+=end.QuadPart-start_.QuadPart;++calls; }
};
}
#define CORDINAL_CORE_SCOPE nativeaudio::benchmark::Scope coreScope
#else
#define CORDINAL_CORE_SCOPE
#endif
