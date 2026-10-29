// Game music playback: opens a chiptune file (SPC/NSF/NSFE/GBS/VGM/VGZ/GYM/SID) into an emulator,
// streams one of its songs through a Decoder, and reads its tags. VGZ is gzip-wrapped VGM and is
// unwrapped first. game_music.h explains the "file|N" song addressing.
#include "game_music.h"

#include "archive.h"
#include "chiptune/chiptune.h"
#include "inflate.h"
#include "library.h"
#include "streamfile.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace {

constexpr unsigned kRate = chiptune::kRate;
constexpr uint64_t kMaxFileBytes = 64ull << 20;  // the biggest rips are a few MB
constexpr int kDefaultPlayMs = 150000;           // for rips that don't give a length
constexpr int kFadeMs = 8000;                    // songs fade out over their last 8 seconds

using Emu = std::unique_ptr<chiptune::Emulator>;

// The whole file, with gzip (VGZ) unwrapped first. Gunzip checks the CRC and length in the
// gzip trailer, so a damaged file fails here.
bool LoadBytes(const std::wstring& file, std::string& bytes) {
    std::string raw;
    FileSource src(file);
    if (src.Valid()) {
        if (src.Size() == 0 || src.Size() > kMaxFileBytes || !src.Read(0, (size_t)src.Size(), raw)) return false;
    } else {  // a rip inside a ZIP pack (archive.h)
        auto member = ReadArchiveMember(file);
        if (!member || member->empty() || member->size() > kMaxFileBytes) return false;
        raw = *member;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(raw.data());
    if (!IsGzip(p, raw.size())) {
        bytes = std::move(raw);
        return true;
    }
    return Gunzip(p, raw.size(), kMaxFileBytes, bytes) && !bytes.empty();
}

// Loads a file into an emulator. The header picks the format.
bool OpenEmu(const std::wstring& file, Emu& emu, std::wstring& error) {
    std::string bytes;
    if (!LoadBytes(file, bytes)) {
        error = L"it can't be read";
        return false;
    }
    std::string why;
    emu = chiptune::Open(bytes, why);
    if (!emu) {
        error = FromUtf8(why);
        return false;
    }
    return true;
}

bool ValidSong(const chiptune::Emulator& emu, int subsong) { return subsong >= 1 && subsong <= emu.TrackCount(); }

// A song's text and how long to play it: its length, else its intro and two passes of its
// loop, else the default.
struct SongInfo {
    chiptune::TrackInfo text;
    int playMs = 0;
};

bool ReadInfo(const chiptune::Emulator& emu, int index, SongInfo& out) {
    if (!emu.Info(index, out.text)) return false;
    const chiptune::TrackInfo& t = out.text;
    out.playMs = t.lengthMs > 0 ? t.lengthMs : t.introMs + 2 * t.loopMs;
    if (out.playMs <= 0) out.playMs = kDefaultPlayMs;
    return true;
}

// The library codec tag for an emulated format.
Codec CodecOf(chiptune::Format f) {
    switch (f) {
        case chiptune::Format::SPC: return Codec::SPC;
        case chiptune::Format::NSF:
        case chiptune::Format::NSFE: return Codec::NSF;
        case chiptune::Format::GBS: return Codec::GBS;
        case chiptune::Format::SID: return Codec::SID;
        default: return Codec::VGM;  // VGM, VGZ and GYM
    }
}

// The first 19xx or 20xx year in a copyright line such as "1991 Nintendo".
std::string YearOf(const std::string& text) {
    for (size_t i = 0; i + 4 <= text.size(); ++i) {
        if (text.compare(i, 2, "19") != 0 && text.compare(i, 2, "20") != 0) continue;
        auto digit = [&](size_t at) { return at < text.size() && text[at] >= '0' && text[at] <= '9'; };
        if (!digit(i + 1) || !digit(i + 2) || !digit(i + 3)) continue;
        if (digit(i + 4) || (i > 0 && digit(i - 1))) continue;
        return text.substr(i, 4);
    }
    return {};
}

// Streams one song. The song's own play length bounds it, so a song that never ends still
// stops; the last kFadeMs fade out.
class GameMusicDecoder final : public Decoder {
public:
    GameMusicDecoder(Emu emu, int track, uint64_t length)
        : emu_(std::move(emu)), track_(track), length_(length),
          fadeStart_(length > (uint64_t)kFadeMs * kRate / 1000 ? length - (uint64_t)kFadeMs * kRate / 1000 : length) {}

    size_t Read(float* out, size_t frames) override {
        if (pos_ >= length_ || emu_->Ended()) return 0;
        frames = (size_t)std::min<uint64_t>(frames, length_ - pos_);
        emu_->Render(out, frames);
        // Fade on a squared curve, which sounds even, reaching silence at the end.
        if (pos_ + frames > fadeStart_) {
            const double span = (double)(length_ - fadeStart_);
            for (size_t i = (size_t)(std::max(pos_, fadeStart_) - pos_); i < frames; ++i) {
                const double left = 1.0 - (double)(pos_ + i - fadeStart_) / span;
                const float gain = (float)(left * left);
                out[i * 2] *= gain;
                out[i * 2 + 1] *= gain;
            }
        }
        // Loud passages can pass full scale; the consoles' outputs saturate there too.
        for (size_t i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
        pos_ += frames;
        return frames;
    }

    bool Seek(double seconds) override {
        const double maxSeconds = length_ / (double)kRate;
        const uint64_t target = (uint64_t)(std::clamp(seconds, 0.0, maxSeconds) * kRate);
        // Emulators only run forward: going back restarts the song.
        if (target < pos_) {
            if (!emu_->Start(track_)) return false;
            pos_ = 0;
        }
        emu_->Skip((size_t)(target - pos_));
        pos_ = target;
        return true;
    }

    double Duration() const override { return length_ / (double)kRate; }
    unsigned Rate() const override { return kRate; }

private:
    Emu emu_;
    int track_;
    uint64_t length_;     // frames
    uint64_t fadeStart_;  // frames; equal to length_ when the song is too short to fade
    uint64_t pos_ = 0;
};

}  // namespace

// Splits "file.nsf|N"; without a "|" the whole path is song 1.
bool SplitGameMusicPath(const std::wstring& path, std::wstring& file, int& subsong) {
    const size_t bar = path.rfind(L'|');
    if (bar == std::wstring::npos) {
        file = path;
        subsong = 1;
        return true;
    }
    const std::wstring digits = path.substr(bar + 1);
    if (digits.empty() || digits.size() > 4 ||
        !std::all_of(digits.begin(), digits.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
        return false;
    int n = 0;
    for (wchar_t c : digits) n = n * 10 + (c - L'0');
    if (n < 1) return false;
    file = path.substr(0, bar);
    subsong = n;
    return true;
}

bool IsGameMusicFile(const std::wstring& path) {
    std::wstring file;
    int subsong = 1;
    if (!SplitGameMusicPath(path, file, subsong)) return false;
    static const wchar_t* const kExt[] = {L"spc", L"nsf", L"nsfe", L"gbs", L"vgm", L"vgz", L"gym", L"sid"};
    const std::wstring ext = ExtOf(file);
    return std::any_of(std::begin(kExt), std::end(kExt), [&](const wchar_t* e) { return ext == e; });
}

// Songs in the file, or 0 if it can't be read.
int GameMusicTrackCount(const std::wstring& file) {
    Emu emu;
    std::wstring error;
    return OpenEmu(file, emu, error) ? emu->TrackCount() : 0;
}

// Opens the emulator and song and fixes the song's play length; the result is the streaming decoder.
std::unique_ptr<Decoder> OpenGameMusicDecoder(const std::wstring& file, int subsong, std::wstring& error) {
    const std::wstring name = FileNameOf(file);
    Emu emu;
    std::wstring why;
    if (!OpenEmu(file, emu, why)) {
        error = L"Couldn't play " + name + L": " + why;
        return nullptr;
    }
    if (!ValidSong(*emu, subsong)) {
        error = L"Couldn't play " + name + L": there is no song " + std::to_wstring(subsong) + L".";
        return nullptr;
    }
    SongInfo info;
    if (!ReadInfo(*emu, subsong - 1, info) || !emu->Start(subsong - 1)) {
        error = L"Couldn't play " + name + L": the song can't be started.";
        return nullptr;
    }
    const uint64_t length = (uint64_t)info.playMs * kRate / 1000;
    return std::make_unique<GameMusicDecoder>(std::move(emu), subsong - 1, length);
}

// The tags a chiptune rip carries: title/song, game, author, copyright year, system and dumper.
bool ReadGameMusicTags(const std::wstring& file, int subsong, TagData& out) {
    Emu emu;
    std::wstring error;
    if (!OpenEmu(file, emu, error) || !ValidSong(*emu, subsong)) return false;
    SongInfo info;
    if (!ReadInfo(*emu, subsong - 1, info)) return false;
    const chiptune::TrackInfo& t = info.text;

    out.title = t.song.empty() ? "Track " + std::to_string(subsong) : t.song;
    out.album = t.game;
    out.artist = t.author;
    out.albumArtist = t.author;
    out.date = YearOf(t.copyright);
    out.genre = "Game";
    out.comment = chiptune::SystemName(emu->Type());  // the console, e.g. "Nintendo NES"
    if (!t.dumper.empty()) out.comment += ", dumped by " + t.dumper;
    out.track = (uint32_t)subsong;
    out.trackTotal = (uint32_t)emu->TrackCount();
    out.duration = info.playMs / 1000.0;
    out.sampleRate = kRate;
    out.channels = 2;
    out.codec = CodecOf(emu->Type());
    return true;
}
