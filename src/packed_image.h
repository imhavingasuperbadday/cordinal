// UPX-packed releases: restores the per-section page protections the packer flattened, before any
// application code runs. cdpack builds: gives cold code back and names the loader's memory.
#pragma once
#include <cstddef>
#include <cstdint>

// Native and independently packed CDP1 builds leave layout.count == 0: CDP1's loader has already
// restored protections before any original TLS callback or app entry. UPX builds restore here.
void RestorePackedImageProtection();

// cdpack's cold code: decommits every cold chunk nothing has touched since the working set was
// last trimmed and nothing is running, and returns how many. They decode again on their next use.
// 0 in a native build or one without cold code.
unsigned ReleaseColdCode();

// The packed image a detached cdpack build was loaded from, whose pages the loader owns (its
// state, the TLS template), or false: GetModuleHandle(nullptr) is the program in every build.
bool PackedLoaderImage(uintptr_t& base, size_t& bytes);

// cdpack's cold code now: its chunks, how many are committed and their bytes, and how often chunks
// were decoded and decommitted. False in a native build or one without cold code.
struct ColdCode {
    unsigned chunks = 0, resident = 0, decodes = 0, evictions = 0;
    size_t residentBytes = 0;
};
bool ColdCodeNow(ColdCode& out);
