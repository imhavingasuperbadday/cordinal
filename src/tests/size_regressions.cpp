// Compatibility checks for the size-oriented parser, sort adapters and metadata registry.
// Called from selftest.cpp's "size regressions" group (and usable standalone). These verify that the
// size-tuned build still behaves like the standard library it replaced: W^X pages, C++ exceptions
// with unwinding, the auto-playlist number parser, ID3 aliases, and the icon.
#include "app.h"
#include "testlog.h"

#include <cstdio>
#include <memory>

namespace {
// Walks the loaded image's committed pages and fails if any is both writable and executable (W^X),
// which a mis-built image (missing NXCOMPAT/DEP) would allow.
bool ImagePagesProtected() {
    auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const BYTE* cursor = base;
    while (cursor < base + nt->OptionalHeader.SizeOfImage) {
        MEMORY_BASIC_INFORMATION page{};
        if (!VirtualQuery(cursor, &page, sizeof(page))) return false;
        DWORD access = page.Protect & 0xff;
        if (page.State == MEM_COMMIT && (access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY))
            return false;
        cursor = static_cast<const BYTE*>(page.BaseAddress) + page.RegionSize;
    }
    return true;
}

// Throws while a RAII local is alive, so the caller can prove exceptions still unwind and run
// destructors under the size-oriented build settings.
__declspec(noinline) void ThrowWithOwnedScope(int& destroyed) {
    struct Scope { int& count; ~Scope() { ++count; } } scope{destroyed};
    throw 0x434f5244u;
}

// Appends a big-endian integer of `bytes` width; syncsafe packs 7 bits per byte, as ID3v2.4 sizes do.
void BigEndian(std::string& out, size_t value, int bytes, bool syncsafe = false) {
    int bits = syncsafe ? 7 : 8;
    for (int i = bytes - 1; i >= 0; --i) out += (char)((value >> (i * bits)) & ((1u << bits) - 1));
}

// Builds a metadata-only ID3v2.2/2.3/2.4 file (a tag with no MPEG frames) for each version and
// checks the reader maps every legacy frame alias (TT2/TIT2, ...) to the same modern fields.
bool CheckId3Aliases(const std::wstring& scratch) {
    bool ok = true;
    for (int version : {2, 3, 4}) {
        std::string frames;
        auto frame = [&](const char* modern, const char* legacy, const std::string& value) {
            frames += version == 2 ? legacy : modern;
            BigEndian(frames, value.size(), version == 2 ? 3 : 4, version == 4);
            if (version != 2) frames.append(2, '\0');
            frames += value;
        };
        auto text = [&](const char* modern, const char* legacy, const char* value) {
            frame(modern, legacy, std::string(1, '\0') + value);
        };
        text("TIT2", "TT2", "Title"); text("TPE1", "TP1", "Artist");
        text("TALB", "TAL", "Album"); text("TPE2", "TP2", "Album artist");
        text("TCON", "TCO", "17"); text("TRCK", "TRK", "7/12"); text("TPOS", "TPA", "2/3");
        text("TCOM", "TCM", "Composer"); text("TIT1", "TT1", "Group"); text("TPUB", "TPB", "Publisher");
        text("TBPM", "TBP", "123.6"); text("TCMP", "TCP", "1"); text("TYER", "TYE", "2005-03-28T12:00");
        frame("COMM", "COM", std::string("\0eng\0Comment", 12));
        frame("USLT", "ULT", std::string("\0eng\0Lyrics", 11));
        std::string bytes = "ID3";
        bytes += (char)version;
        bytes.append(2, '\0');
        BigEndian(bytes, frames.size(), 4, true);
        bytes += frames;
        std::wstring path = scratch + L"\\size-id3-" + std::to_wstring(version) + L".mp3";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        bool saved = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr)
                     && written == bytes.size();
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        TagData tags;
        ReadTags(path, tags);  // This metadata-only fixture deliberately has no MPEG audio frames.
        std::string lyrics;
        ReadLyrics(path, lyrics);
        ok &= saved && tags.title == "Title" && tags.artist == "Artist" && tags.album == "Album" &&
              tags.albumArtist == "Album artist" && tags.genre == "Rock" && tags.track == 7 && tags.trackTotal == 12 &&
              tags.disc == 2 && tags.discTotal == 3 && tags.composer == "Composer" && tags.grouping == "Group" &&
              tags.label == "Publisher" && tags.bpm == 124 && tags.compilation && tags.date == "2005-03-28" &&
              tags.comment == "Comment" && lyrics == "Lyrics";
        DeleteFileW(path.c_str());
    }
    return ok;
}
}  // namespace

int TestSizeRegressions(const std::wstring& scratch, FILE* log) {
    int failures = 0;
    testlog::SetLog(log);
    auto check = [&](bool ok, const wchar_t* message) {
        testlog::Check(ok, L"SIZE %ls", message);
        failures += !ok;
    };
    check(ImagePagesProtected(), L"image pages are never both writable and executable after startup");
    int destroyed = 0;
    bool unwound = false;
    try { ThrowWithOwnedScope(destroyed); }
    catch (unsigned value) { unwound = value == 0x434f5244u && destroyed == 1; }
    check(unwound, L"C++ exceptions retain stack unwinding, typed catches and RAII cleanup");
    // The custom sorts are held to std::sort / std::stable_sort by the sorting suite (selftest_workflows.cpp).

    // Legacy auto-playlist limit lines: partial words, a sign, an overflow and a missing field must
    // all still parse exactly as the original parser did.
    struct Case { const char* line; uint32_t limit; bool minutes, ordered; PickBy pick; };
    bool parsing = true;
    for (const Case& c : {
        Case{"limit +17minutes mostplayed 1", 17, true, true, PickBy::MostPlayed},
        Case{"limit -1 tracks leastplayed 1", UINT32_MAX, false, true, PickBy::LeastPlayed},
        Case{"limit 4294967296 minutes mostplayed 1", UINT32_MAX, false, false, PickBy::Random},
        Case{"limit 23 tracks mostplayed 1\nlimit", 23, false, false, PickBy::Random},
        Case{"limit 0x10 tracks mostplayed 1", 0, false, false, PickBy::Random},
    }) {
        AutoPlaylist playlist;
        parsing &= ParseAutoPlaylist(std::string("cordinal-auto 1\n") + c.line + "\ngroup all\nend\n", playlist) &&
                   playlist.limit == c.limit && (playlist.unit == LimitUnit::Minutes) == c.minutes && playlist.ordered == c.ordered &&
                   playlist.pick == c.pick;
    }
    check(parsing, L"playlist numeric extraction retains legacy partial, signed, overflow and missing-field behavior");
    check(CheckId3Aliases(scratch), L"ID3v2.2/2.3/2.4 metadata aliases retain fields, number pairs, comments and lyrics");

    bool icons = true;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    for (int size : {16, 20, 24, 32, 40, 48, 64, 72, 96, 128, 256}) {
        HICON icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, 0);
        icons &= icon != nullptr;
        if (icon) DestroyIcon(icon);
    }
    check(icons, L"application icon loads at all tested DPI sizes, including scaled 72/96/128 px");
    return failures;
}
