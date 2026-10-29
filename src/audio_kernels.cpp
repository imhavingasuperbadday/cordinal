// Runtime CPU dispatch for the audio SIMD kernels. FastAudioKernels() is the one
// gate every caller consults: false selects the SSE2 baseline, true the AVX2/FMA
// implementations compiled in the AVX2 translation unit.
#include "audio_kernels.h"
#ifdef _MSC_VER
#include <intrin.h>
#endif

bool nativeaudio::FastAudioKernels() {
#if defined(CORDINAL_AUDIO_NO_AVX2)
    return false; // Standalone verification of the baseline path on AVX2 machines.
#else
    // All three flags are tested together before any YMM state is touched; the
    // result is cached for the process lifetime.
    static const bool fast = [] {
#ifdef _MSC_VER
        int cpu[4]{};
        __cpuid(cpu,0);
        if (cpu[0] < 7) return false;
        __cpuidex(cpu,1,0);
        constexpr int needed = (1 << 27) | (1 << 28) | (1 << 12); // OSXSAVE, AVX, FMA
        if ((cpu[2] & needed) != needed || (_xgetbv(0) & 6) != 6) return false;
        __cpuidex(cpu,7,0);
        return (cpu[1] & (1 << 5)) != 0;
#else
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
    }();
    return fast;
#endif
}
bool nativeaudio::FastCrcKernel() {
    static const bool fast = [] {
        if (!FastAudioKernels()) return false;
#ifdef _MSC_VER
        int cpu[4]{};
        __cpuidex(cpu,1,0);
        return (cpu[2] & (1 << 1)) != 0; // PCLMULQDQ
#else
        return __builtin_cpu_supports("pclmul") != 0;
#endif
    }();
    return fast;
}
