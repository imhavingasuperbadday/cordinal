// Force relocation, call an exported function after TLS initialization, then unload safely.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    const auto preferred = uintptr_t(std::strtoull(argv[2], nullptr, 0));
    void *reservation =
        VirtualAlloc(reinterpret_cast<void *>(preferred),
                     size_t(std::strtoull(argv[3], nullptr, 0)), MEM_RESERVE, PAGE_NOACCESS);
    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        std::printf("LoadLibrary error=%lu\n", GetLastError());
        return 3;
    }
    const auto probe = reinterpret_cast<unsigned (*)()>(GetProcAddress(module, "fixture_probe"));
    const unsigned status = probe ? probe() : 99;
    const bool moved = reinterpret_cast<uintptr_t>(module) != preferred;
    if (!FreeLibrary(module))
        return 4;
    if (reservation)
        VirtualFree(reservation, 0, MEM_RELEASE);
    std::printf("DLL status=%u relocated=%u\n", status, moved);
    return status || !moved ? 5 : 0;
}
