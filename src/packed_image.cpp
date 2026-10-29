// UPX compresses a release into one read-write-execute image, which forfeits the native per-section
// protections. tools/pack-release.ps1 records each original region's RVA, size and protection in a
// reserved `.rdata$layout` table before compressing; this code re-applies them at startup. Native
// builds leave the count at zero and do nothing.
// The independent CDP1 loader restores native protections before TLS callbacks or entry; that
// build also leaves this table empty. tools/pack-cdpack.ps1 verifies its complete native image.
#include "packed_image.h"
#include <windows.h>
#include <cstring>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
// One recorded native section region: where it lived, how big, and its original protection.
struct Region { DWORD rva, size, protection; };
// `marker` is a sanity tag the packer writes alongside the table; `count` is 0 until then.
struct Layout { BYTE marker[16]; DWORD count; Region regions[16]; };

// tools/pack-release.ps1 fills this reserved table after linking, before compression. Volatile
// prevents LTCG from assuming the native build's zero count. No UPX code or stub is modified.
#pragma section(".rdata$layout", read)
__declspec(allocate(".rdata$layout")) const volatile Layout layout{
    {0x8c, 0x53, 0x21, 0x97, 0x6a, 0xc4, 0x48, 0x5b, 0xb0, 0x1f, 0x43, 0x4f, 0x52, 0x44, 0x01, 0x00}, 0, {}};

void Protect(BYTE* base, DWORD rva, DWORD size, DWORD protection) {
    DWORD old;
    if (size) VirtualProtect(base + rva, size, protection, &old);
}
}  // namespace

// Re-applies the recorded protections, then hardens whatever the packer added: the resource region
// becomes read-only and no page is left both writable and executable. Call only for a packed image,
// which is recognized by the three sections a UPX stub introduces.
void RestorePackedImageProtection() {
    DWORD count = layout.count;
    if (!count || count > 16) return;  // native/CDP1 build, or a table we don't trust
    auto* base = reinterpret_cast<BYTE*>(&__ImageBase);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
    auto* sections = IMAGE_FIRST_SECTION(nt);
    // `upx -d` restores the native layout while retaining our table; it needs no protection fix.
    if (nt->FileHeader.NumberOfSections != 3 || memcmp(sections->Name, "UPX0", 4)) return;
    DWORD cursor = layout.regions[0].rva;
    for (DWORD i = 0; i < count; ++i) {
        DWORD rva = layout.regions[i].rva, size = layout.regions[i].size;
        Protect(base, cursor, rva - cursor, PAGE_READONLY);
        Protect(base, rva, size, layout.regions[i].protection);
        cursor = rva + size;
    }
    // Original code, constants, data and unwind tables retain their RVAs. UPX moves resources
    // and adds a loader after them: leave that loader executable for its TLS callback, and
    // make the resource region read-only. No page remains both writable and executable.
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = sections[i];
        DWORD end = s.VirtualAddress + s.Misc.VirtualSize;
        if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            DWORD begin = cursor > s.VirtualAddress ? cursor : s.VirtualAddress;
            if (end > begin) Protect(base, begin, end - begin, PAGE_EXECUTE_READ);
        } else {
            Protect(base, s.VirtualAddress, s.Misc.VirtualSize, PAGE_READONLY);
        }
    }
}
