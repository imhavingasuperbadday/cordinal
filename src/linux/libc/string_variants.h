// Both builds of string.cpp's dispatched functions, so the checks can run the SSE2 versions on a
// processor where the program itself picks AVX2.
#pragma once
#include <stddef.h>

struct CordinalStringFunctions {
    void* (*memcpy)(void*, const void*, size_t);
    void* (*memmove)(void*, const void*, size_t);
    void* (*memset)(void*, int, size_t);
    int (*memcmp)(const void*, const void*, size_t);
    const void* (*memchr)(const void*, int, size_t);
    size_t (*strlen)(const char*);
    const char* (*strchr)(const char*, int);
    int (*strcmp)(const char*, const char*);
    int (*strncmp)(const char*, const char*, size_t);
};

// The SSE2 build (avx2 = 0) or the AVX2 build (avx2 = 1); null when this processor can't run it.
extern "C" __attribute__((visibility("hidden"))) const CordinalStringFunctions* cordinal_string_functions(int avx2);
