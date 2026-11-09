// Attributes every resident private page to an owner: the program image, the heaps, thread stacks,
// Windows DLLs, mapped files or other private allocations. The system's own private working set is
// the total; the walk explains it, so a regression names what grew instead of only how much.
#define WIN32_LEAN_AND_MEAN
#include "memory_report.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <map>

namespace {

constexpr size_t kPage = 4096;

struct Region {
    uintptr_t begin, end, allocation;
    DWORD type;
};

// Heap blocks counted by exact size: a cache of covers shows up as many blocks of one size, a buffer
// as one large block, strings as many small ones. Open addressing in storage reserved beforehand.
struct SizeCounts {
    std::vector<std::pair<size_t, size_t>> slots = std::vector<std::pair<size_t, size_t>>(8192);
    void Add(size_t size) {
        for (size_t i = (size * 0x9E3779B97F4A7C15ull) >> 51, probes = 0; probes < slots.size(); ++probes, i = (i + 1) % slots.size()) {
            auto& slot = slots[i];
            if (slot.second && slot.first != size) continue;
            slot.first = size;
            ++slot.second;
            return;
        }
    }
};

// Allocation bases of the heaps' segments and of their separately reserved large blocks. Nothing may
// allocate while a heap is locked (the segment heap's lock is not reentrant), so the caller reserves.
void HeapBases(std::vector<uintptr_t>& bases, size_t& inUse, SizeCounts& sizes) {
    HANDLE heaps[64];
    const DWORD count = std::min<DWORD>(GetProcessHeaps(64, heaps), 64);
    for (DWORD h = 0; h < count; ++h) {
        if (!HeapLock(heaps[h])) continue;
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heaps[h], &entry)) {
            const bool region = entry.wFlags & PROCESS_HEAP_REGION;
            const bool busy = entry.wFlags & PROCESS_HEAP_ENTRY_BUSY;
            if (busy) inUse += entry.cbData, sizes.Add(entry.cbData);
            // A region starts a segment; a busy block this large may be a block reserved on its own.
            if ((region || (busy && entry.cbData >= 64 * 1024)) && bases.size() < bases.capacity()) {
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(entry.lpData, &info, sizeof(info)))
                    bases.push_back((uintptr_t)info.AllocationBase);
            }
        }
        HeapUnlock(heaps[h]);
    }
    bases.push_back((uintptr_t)heaps[0]);
}

// Allocation bases of every thread's stack and thread block.
unsigned ThreadBases(std::vector<uintptr_t>& bases) {
    struct BasicInformation {
        LONG exitStatus;
        PVOID teb;
        PVOID clientId[2];
        ULONG_PTR affinity;
        LONG priority, basePriority;
    };
    using Query = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query =
        reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    unsigned threads = 0;
    const DWORD self = GetCurrentProcessId();
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != self) continue;
        ++threads;
        HANDLE thread = query ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID) : nullptr;
        if (!thread) continue;
        BasicInformation basic{};
        if (query(thread, 0, &basic, sizeof(basic), nullptr) >= 0 && basic.teb) {
            // The thread block is ours to read; its first fields are the stack's bounds.
            const auto* tib = static_cast<const NT_TIB*>(basic.teb);
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(basic.teb, &info, sizeof(info))) bases.push_back((uintptr_t)info.AllocationBase);
            if (VirtualQuery((const char*)tib->StackBase - 1, &info, sizeof(info)))
                bases.push_back((uintptr_t)info.AllocationBase);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return threads;
}

std::wstring ModuleName(uintptr_t base) {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW((HMODULE)base, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return L"(unnamed image)";
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

double Mb(size_t bytes) { return bytes / 1048576.0; }

}  // namespace

MemoryReport TakeMemoryReport() {
    MemoryReport r;
    PROCESS_MEMORY_COUNTERS_EX2 counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters))) {
        r.privateWorkingSet = counters.PrivateWorkingSetSize;
        r.privateBytes = counters.PrivateUsage;
        r.workingSet = counters.WorkingSetSize;
    }
    std::vector<uintptr_t> heapBases, threadBases;
    heapBases.reserve(4096);
    threadBases.reserve(512);
    SizeCounts sizes;
    HeapBases(heapBases, r.heapInUse, sizes);
    for (const auto& slot : sizes.slots)
        if (slot.second) r.topBlocks.push_back(slot);
    std::sort(r.topBlocks.begin(), r.topBlocks.end(),
              [](const auto& a, const auto& b) { return a.first * a.second > b.first * b.second; });
    if (r.topBlocks.size() > 10) r.topBlocks.resize(10);
    r.threads = ThreadBases(threadBases);
    std::sort(heapBases.begin(), heapBases.end());
    std::sort(threadBases.begin(), threadBases.end());

    std::vector<Region> regions;
    MEMORY_BASIC_INFORMATION info{};
    for (uintptr_t at = 0; VirtualQuery((void*)at, &info, sizeof(info)); at = (uintptr_t)info.BaseAddress + info.RegionSize) {
        if (info.State == MEM_COMMIT)
            regions.push_back({(uintptr_t)info.BaseAddress, (uintptr_t)info.BaseAddress + info.RegionSize,
                               (uintptr_t)info.AllocationBase, info.Type});
        if ((uintptr_t)info.BaseAddress + info.RegionSize <= at) break;
    }

    // The working set list: one entry per resident page. The size changes between the two calls,
    // so ask with room to spare. VirtualAlloc keeps this scratch out of the heaps being measured.
    PSAPI_WORKING_SET_INFORMATION probe{};
    QueryWorkingSet(GetCurrentProcess(), &probe, sizeof(probe));
    const size_t entries = probe.NumberOfEntries + 4096;
    const size_t bytes = sizeof(ULONG_PTR) + entries * sizeof(PSAPI_WORKING_SET_BLOCK);
    auto* list = static_cast<PSAPI_WORKING_SET_INFORMATION*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT, PAGE_READWRITE));
    if (!list) return r;
    const uintptr_t program = (uintptr_t)GetModuleHandleW(nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(program);
    const size_t programSize = reinterpret_cast<const IMAGE_NT_HEADERS*>(program + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    std::map<uintptr_t, size_t> libraries;
    if (QueryWorkingSet(GetCurrentProcess(), list, (DWORD)bytes)) {
        for (ULONG_PTR i = 0; i < list->NumberOfEntries; ++i) {
            const PSAPI_WORKING_SET_BLOCK& page = list->WorkingSetInfo[i];
            const uintptr_t address = (uintptr_t)page.VirtualPage * kPage;
            if (address - program < programSize) r.programResident += kPage;
            if (page.Shared) continue;
            auto region = std::upper_bound(regions.begin(), regions.end(), address,
                                           [](uintptr_t a, const Region& x) { return a < x.begin; });
            if (region == regions.begin() || address >= (--region)->end) {
                r.buffers += kPage;  // committed after the walk
                continue;
            }
            const uintptr_t base = region->allocation;
            if (region->type == MEM_IMAGE) {
                if (base == program) r.program += kPage;
                else r.libraries += kPage, libraries[base] += kPage;
            } else if (region->type == MEM_MAPPED) {
                r.mapped += kPage;
            } else if (std::binary_search(heapBases.begin(), heapBases.end(), base)) {
                r.heaps += kPage;
            } else if (std::binary_search(threadBases.begin(), threadBases.end(), base)) {
                r.stacks += kPage;
            } else {
                r.buffers += kPage;
            }
        }
    }
    VirtualFree(list, 0, MEM_RELEASE);
    for (auto& [base, size] : libraries) r.topLibraries.emplace_back(ModuleName(base), size);
    std::sort(r.topLibraries.begin(), r.topLibraries.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (r.topLibraries.size() > 8) r.topLibraries.resize(8);
    return r;
}

std::wstring FormatMemoryReport(const MemoryReport& r) {
    wchar_t line[256];
    std::wstring out;
    auto add = [&](const wchar_t* format, auto... values) {
        swprintf_s(line, format, values...);
        out += line;
    };
    add(L"memory %.2f MB (private working set, the figure Task Manager shows)\n", Mb(r.privateWorkingSet));
    add(L"  program     %6.2f MB  cordinal.exe's own pages (%.2f MB resident in all)\n", Mb(r.program),
        Mb(r.programResident));
    add(L"  heaps       %6.2f MB  %.2f MB allocated\n", Mb(r.heaps), Mb(r.heapInUse));
    add(L"  buffers     %6.2f MB  graphics, audio and other direct allocations\n", Mb(r.buffers));
    add(L"  stacks      %6.2f MB  %u threads\n", Mb(r.stacks), r.threads);
    add(L"  libraries   %6.2f MB  Windows DLLs' private pages\n", Mb(r.libraries));
    if (r.mapped) add(L"  mapped      %6.2f MB\n", Mb(r.mapped));
    for (const auto& [name, size] : r.topLibraries) add(L"    %-24ls %6.2f MB\n", name.c_str(), Mb(size));
    out += L"largest heap allocations by size (allocated; a block's untouched pages are not resident)\n";
    for (const auto& [size, count] : r.topBlocks)
        add(L"  %6.2f MB  %zu x %zu bytes\n", Mb(size * count), count, size);
    add(L"committed %.2f MB, working set %.2f MB\n", Mb(r.privateBytes), Mb(r.workingSet));
    return out;
}

bool WriteMemoryReport(const std::wstring& path) {
    const std::wstring text = FormatMemoryReport(TakeMemoryReport());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), utf8.data(), size, nullptr, nullptr);
    DWORD written = 0;
    const bool ok = WriteFile(file, utf8.data(), (DWORD)utf8.size(), &written, nullptr) && written == utf8.size();
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}
