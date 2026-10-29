// Format dispatch and the helpers shared by the loaders: Open() sniffs a file's magic bytes and
// hands it to the matching loader, and SystemName() maps a Format to its console. FieldText()
// decodes the fixed-size text fields the tag blocks use.
#include "chiptune.h"

#include <cstring>

namespace chiptune {

// Scans to the first NUL (or the field end) and trims trailing spaces; other bytes pass through.
std::string FieldText(const unsigned char* p, size_t size) {
    size_t n = 0;
    while (n < size && p[n]) ++n;
    while (n > 0 && p[n - 1] == ' ') --n;
    return std::string((const char*)p, n);
}

std::unique_ptr<Emulator> Open(const std::string& bytes, std::string& error) {
    auto starts = [&](const char* magic) {
        const size_t n = std::strlen(magic);
        return bytes.size() >= n && std::memcmp(bytes.data(), magic, n) == 0;
    };
    if (starts("NESM\x1A")) return OpenNsf(bytes, error);
    if (starts("NSFE")) return OpenNsfe(bytes, error);
    if (starts("GBS")) return OpenGbs(bytes, error);
    if (starts("SNES-SPC700 Sound File Data")) return OpenSpc(bytes, error);
    if (starts("Vgm ")) return OpenVgm(bytes, error);
    if (starts("GYMX")) return OpenGym(bytes, error);
    if (starts("PSID") || starts("RSID")) return OpenSid(bytes, error);
    error = "it isn't a game music file this player knows";
    return nullptr;
}

const char* SystemName(Format f) {
    switch (f) {
        case Format::NSF:
        case Format::NSFE: return "Nintendo NES";
        case Format::GBS: return "Nintendo Game Boy";
        case Format::SPC: return "Super Nintendo";
        case Format::VGM: return "Sega SMS/Genesis";
        case Format::GYM: return "Sega Genesis";
        case Format::SID: return "Commodore 64";
    }
    return "";
}

}  // namespace chiptune
