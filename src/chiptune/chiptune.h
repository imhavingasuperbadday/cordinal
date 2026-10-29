// Video game music emulators: NSF/NSFE (NES), GBS (Game Boy), SPC (SNES), VGM/VGZ (Master
// System, Genesis), GYM (Genesis) and SID (Commodore 64). Each one loads a whole file, plays one of its songs at a
// time and renders interleaved stereo float samples at 44100 Hz.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace chiptune {

// Output rate shared by every emulator and baked into their resamplers.
constexpr int kRate = 44100;

// Text is UTF-8. Lengths are in milliseconds, -1 when the file doesn't say.
struct TrackInfo {
    std::string game, song, author, copyright, dumper;
    int lengthMs = -1;  // the whole song
    int introMs = -1;   // before the looped part
    int loopMs = -1;    // one pass of the looped part
};

enum class Format { NSF, NSFE, GBS, SPC, VGM, GYM, SID };

class Emulator {
public:
    virtual ~Emulator() = default;

    virtual Format Type() const = 0;
    virtual int TrackCount() const = 0;
    // Fills in a track's text and lengths. False if there is no such track.
    virtual bool Info(int track, TrackInfo& out) const = 0;
    // Resets the hardware and starts a track (0-based).
    virtual bool Start(int track) = 0;
    // Overwrites out with frames * 2 samples.
    virtual void Render(float* out, size_t frames) = 0;
    // Runs the song forward without producing sound (for seeking).
    virtual void Skip(size_t frames) = 0;
    // True once a song with a definite end has played to it; Render then gives silence.
    virtual bool Ended() const { return false; }
};

// Opens a file by its header. Returns null with a short reason if it isn't one of the
// formats or is damaged. The emulator keeps its own copy of what it needs.
std::unique_ptr<Emulator> Open(const std::string& bytes, std::string& error);

// The console, as shown in the tag comment, e.g. "Nintendo NES".
const char* SystemName(Format f);

// Little-endian readers and fixed-size text fields, shared by the format loaders.
inline unsigned Le16(const unsigned char* p) { return p[0] | p[1] << 8; }
inline uint32_t Le32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
// Text from a fixed-size, possibly unterminated field, trimmed of spaces at the end.
std::string FieldText(const unsigned char* p, size_t size);

// The loaders, one per file family.
std::unique_ptr<Emulator> OpenNsf(const std::string& bytes, std::string& error);
std::unique_ptr<Emulator> OpenNsfe(const std::string& bytes, std::string& error);
std::unique_ptr<Emulator> OpenGbs(const std::string& bytes, std::string& error);
std::unique_ptr<Emulator> OpenSpc(const std::string& bytes, std::string& error);
std::unique_ptr<Emulator> OpenVgm(const std::string& bytes, std::string& error);
std::unique_ptr<Emulator> OpenGym(const std::string& bytes, std::string& error);
std::unique_ptr<Emulator> OpenSid(const std::string& bytes, std::string& error);

}  // namespace chiptune
