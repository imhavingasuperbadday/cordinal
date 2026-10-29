// Sound effects in the Linux window: PlaySfx for the actions that otherwise happen silently, as the Windows
// build's PlaySfx (app_playback.cpp) and DELTARUNE theme sounds (app_deltarune.cpp) do. The sounds are
// synthesized (sfx.cpp) and played on their own PipeWire stream (sfx_pipewire.cpp); in the DELTARUNE theme
// the game's own menu sounds, embedded as FLAC (game_sounds.inc, made by CMake from src/deltarune), stand
// in for them and play as clips on the same stream. Robotic sounds crush them the way Queen's lines are
// crushed, and speak status messages in her text blip.
#include "app.h"
#include "native_audio.h"
#include "sfx.h"

#include "game_sounds.inc"

#include <cmath>
#include <map>

namespace lx {
namespace {

struct Blob {
    const unsigned char* data;
    size_t size;
};
#define BLOB(name) Blob{kSnd_##name, sizeof(kSnd_##name)}

enum class Res { None, Select, MenuMove, Noise, Save, Bell, Error, CantSelect, Swing, LevelUp, Txtq, QueenLaugh };

Blob BlobOf(Res r) {
    switch (r) {
        case Res::Select: return BLOB(select);
        case Res::MenuMove: return BLOB(menumove);
        case Res::Noise: return BLOB(noise);
        case Res::Save: return BLOB(save);
        case Res::Bell: return BLOB(bell);
        case Res::Error: return BLOB(error);
        case Res::CantSelect: return BLOB(cantselect);
        case Res::Swing: return BLOB(swing);
        case Res::LevelUp: return BLOB(levelup);
        case Res::Txtq: return BLOB(txtq);
        case Res::QueenLaugh: return BLOB(queen_bitcrushlaugh);
        default: return {nullptr, 0};
    }
}

// The game's sound for each of Cordinal's (the table in app_deltarune.cpp).
Res SoundFor(Sfx sound) {
    switch (sound) {
        case Sfx::Rate:
        case Sfx::PlayNext: return Res::Select;
        case Sfx::Unrate:
        case Sfx::QueueAdd: return Res::MenuMove;
        case Sfx::QueueClear:
        case Sfx::Drop: return Res::Noise;
        case Sfx::TagsSaved: return Res::Save;
        case Sfx::Organized:
        case Sfx::NewFiles:
        case Sfx::Sparkle: return Res::Bell;
        case Sfx::Unorganized: return Res::Error;
        case Sfx::Oops: return Res::CantSelect;
        case Sfx::Dice: return Res::Swing;
        case Sfx::Milestone: return RoboticSounds() ? Res::QueenLaugh : Res::LevelUp;
        default: return Res::None;
    }
}

// Queen's crush: each sample held for three, then quantized to 32 levels.
std::vector<float> Bitcrush(const std::vector<float>& stereo) {
    std::vector<float> out(stereo.size());
    float held[2]{};
    for (size_t f = 0; f < stereo.size() / 2; ++f)
        for (int c = 0; c < 2; ++c) {
            if (f % 3 == 0) held[c] = std::round(stereo[f * 2 + c] * 16) / 16;
            out[f * 2 + c] = held[c];
        }
    return out;
}

uint64_t g_ourClipUntil = 0;  // when the last of these clips ends (a little after)

// The games play their music as a clip too: a clip that isn't the theme's sound is left alone, and the
// synthesized sound plays beside it instead.
bool PlayThemeClip(const std::vector<float>& stereo, unsigned rate, float gain) {
    const uint64_t now = SDL_GetTicks();
    if (stereo.empty() || !rate || (sfx::ClipPosition() >= 0 && now > g_ourClipUntil)) return false;
    sfx::PlayClip(stereo, rate, gain);
    g_ourClipUntil = now + stereo.size() / 2 * 1000 / rate + 250;
    return true;
}

// Plays the game's sound `r`, crushed when robotic sounds are on; false when it can't.
bool PlayResource(Res r, float gain) {
    struct Cached {
        Res res;
        unsigned rate = 0;
        std::vector<float> stereo, crushed;
    };
    static std::vector<Cached> cache;  // decoded on first use
    if (r == Res::None) return false;
    auto it = std::find_if(cache.begin(), cache.end(), [&](const Cached& c) { return c.res == r; });
    if (it == cache.end()) {
        Cached c{r};
        const Blob blob = BlobOf(r);
        if (auto decoder = OpenFlacDecoder(blob.data, blob.size)) {
            c.rate = decoder->Rate();
            c.stereo.resize(2 * (size_t)std::llround(decoder->Duration() * c.rate));
            c.stereo.resize(2 * decoder->Read(c.stereo.data(), c.stereo.size() / 2));
        }
        c.crushed = Bitcrush(c.stereo);
        cache.push_back(std::move(c));
        it = cache.end() - 1;
    }
    const bool crush = RoboticSounds() && r != Res::QueenLaugh;  // Queen's laugh is crushed already
    return PlayThemeClip(crush ? it->crushed : it->stereo, it->rate, gain);
}

bool GameSounds() { return ThemeInfo(g.settings.theme).gameSounds; }

// The pack theme in use replaces the sound effects it has a sound for; the rest stay cordinal's.
bool PlayPackThemeSound(Sfx sound, float gain) {
    const PackTheme* theme = PackThemeFor(g.settings.theme);
    if (!theme) return false;
    auto it = std::find_if(theme->sounds.begin(), theme->sounds.end(), [&](auto& x) { return x.first == sound; });
    if (it == theme->sounds.end()) return false;
    struct Clip {
        unsigned rate = 0;
        std::vector<float> stereo;
    };
    static std::map<std::pair<const PackTheme*, Sfx>, Clip> cache;  // decoded when first played
    auto [slot, fresh] = cache.try_emplace({theme, sound});
    Clip& c = slot->second;
    if (fresh) {
        if (!DecodePackWav(it->second, c.rate, c.stereo))
            if (auto d = OpenFlacDecoder((const uint8_t*)it->second.data(), it->second.size())) {
                c.rate = d->Rate();
                c.stereo.resize(2 * (size_t)std::llround(std::min(d->Duration(), 10.0) * c.rate));
                c.stereo.resize(2 * d->Read(c.stereo.data(), c.stereo.size() / 2));
            }
    }
    return !c.stereo.empty() && PlayThemeClip(c.stereo, c.rate, gain);
}

float Level(bool ducked, double scale) {
    const double v = g.settings.volume;
    return (float)(scale * v * v * (ducked ? 0.6 : 1));
}

}  // namespace

bool RoboticSounds() { return g.settings.roboticSounds; }

// Sound effects follow the volume, well below the music, and quieter still while it plays. Exclusive
// output has the device to itself, so they stay silent there.
void PlaySfx(Sfx sound, int arg, bool evenWhenOff) {
    if ((!g.settings.soundEffects && !evenWhenOff) || g.settings.exclusive) return;
    const bool ducked = g.playing != kNone && !g.engine.Paused();
    if (PlayPackThemeSound(sound, Level(ducked, 0.3))) return;
    if (GameSounds() && PlayResource(SoundFor(sound), Level(ducked, 0.3))) return;
    sfx::Play(sound, arg, ducked, Level(ducked, 0.4));
}

// A status message in the DELTARUNE theme with robotic sounds, spoken in Queen's blip; at most one every
// second and a half, so a burst of messages doesn't chatter.
void MessageBlip() {
    static uint64_t last = 0;
    const uint64_t now = SDL_GetTicks();
    if (!GameSounds() || !RoboticSounds() || !g.settings.soundEffects || g.settings.exclusive || now - last < 1500 ||
        (now < g_ourClipUntil && sfx::ClipPosition() >= 0))
        return;
    last = now;
    PlayResource(Res::Txtq, Level(g.playing != kNone && !g.engine.Paused(), 0.3));
}

void ShutdownSounds() { sfx::Shutdown(); }

}  // namespace lx
