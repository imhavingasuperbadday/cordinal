// What the private working set (the "Memory" Task Manager shows) is made of, page by page and by
// owner, and the budgets the memory-budget suite holds each owner to. See docs/memory.md.
#pragma once
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Every resident private page belongs to exactly one owner, so the owners add up to the total
// (within the few pages that change between the system's count and the walk).
struct MemoryReport {
    size_t privateWorkingSet = 0;  // Task Manager's figure, as the system reports it
    size_t privateBytes = 0;       // committed private memory, resident or not
    size_t workingSet = 0;         // every resident page, including shared Windows code
    size_t program = 0;            // cordinal.exe's own pages; a packed build holds all of its code privately
    size_t programResident = 0;    // every resident page of cordinal.exe, shared or not: in a native build,
                                   // the code actually used, which Windows shares from the file
    size_t heaps = 0;              // process heaps: C++ allocations of cordinal and of Windows libraries
    size_t heapInUse = 0;          // bytes those heaps have handed out (the rest is free space they keep)
    size_t buffers = 0;            // other private allocations: graphics devices, audio buffers, large blocks
    size_t stacks = 0;             // thread stacks and thread blocks
    size_t libraries = 0;          // private pages of Windows DLLs (their data and relocations)
    size_t mapped = 0;             // private pages of mapped files
    unsigned threads = 0;
    std::vector<std::pair<std::wstring, size_t>> topLibraries;  // largest DLLs, by private bytes
    std::vector<std::pair<size_t, size_t>> topBlocks;  // heap block size and count, largest totals first
};

// WM_COPYDATA tag: the payload is a path, and the running player writes its report there. The
// memory-budget suite and `cordinal-test --memory-report` ask the real player this way.
constexpr unsigned long kCopyDataMemoryReport = 0x43444D52;  // 'CDMR'

// Walks the address space and the working set; a few milliseconds, safe on any thread. Walking the
// heaps reads a header on every page that holds a block, so the report pages the whole heap back in
// and raises the figure it reports; Help > About uses TakeMemoryFigures instead.
MemoryReport TakeMemoryReport();

// Task Manager's figure and the program's share, from the system's counters and the working-set
// list alone: nothing of the process is read, so asking leaves the figure as it was.
struct MemoryFigures {
    size_t privateWorkingSet = 0;
    size_t program = 0;
};
MemoryFigures TakeMemoryFigures();
// Task Manager's figure alone, in one system call. Before PROCESS_MEMORY_COUNTERS_EX2 (older
// Windows 10) Windows has no private figure, so this is the whole working set there: still fine
// for spotting growth, which is what Lightweight mode's watch uses it for.
size_t PrivateWorkingSetNow();
std::wstring FormatMemoryReport(const MemoryReport& report);
// Writes the report to a new file (never replacing one); false when the file exists or fails.
bool WriteMemoryReport(const std::wstring& path);
