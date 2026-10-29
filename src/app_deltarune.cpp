// The DELTARUNE theme's sound effects: the game's own menu sounds, built into the executable
// (src/deltarune, cordinal.rc), stand in for Cordinal's synthesized ones (sfx.h):
//   rating, play next          snd_select      the menu's confirm
//   unrating, adding to queue  snd_menumove    the cursor moving
//   clearing the queue, drops  snd_noise
//   tags saved                 snd_save        a SAVE point
//   organized, new files, new  snd_bell
//   auto-playlist
//   undoing an organize        snd_error
//   something went wrong       snd_cantselect
//   shuffle's dice             snd_swing
//   a play-count milestone     snd_levelup
// The cardinal's song stays the cardinal's. The sounds play as clips on the sound effect stream.
//
// Robotic sounds (Preferences > View, on by default) give them Chapter 2's Cyber World voice: every
// sound is bitcrushed the way Queen's lines are (held to a third of the rate, 5-bit), a milestone
// gets Queen's bitcrushed laugh, and status messages speak with her text blip (snd_txtq).
#include "app.h"
#include "native_audio.h"
#include "resource.h"

namespace app {

namespace {

int SoundFor(Sfx sound) {
    switch (sound) {
        case Sfx::Rate:
        case Sfx::PlayNext: return IDR_SND_SELECT;
        case Sfx::Unrate:
        case Sfx::QueueAdd: return IDR_SND_MENUMOVE;
        case Sfx::QueueClear:
        case Sfx::Drop: return IDR_SND_NOISE;
        case Sfx::TagsSaved: return IDR_SND_SAVE;
        case Sfx::Organized:
        case Sfx::NewFiles:
        case Sfx::Sparkle: return IDR_SND_BELL;
        case Sfx::Unorganized: return IDR_SND_ERROR;
        case Sfx::Oops: return IDR_SND_CANTSELECT;
        case Sfx::Dice: return IDR_SND_SWING;
        case Sfx::Milestone: return RoboticSounds() ? IDR_SND_QUEEN_BITCRUSHLAUGH : IDR_SND_LEVELUP;
        default: return 0;
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

bool s_robotic = true;  // LoadTweaks sets the saved choice
ULONGLONG ourClipUntil = 0;  // when the last of these clips ends (a little after)

// Plays the sound resource `id` as a clip, robotic when that's on; false when it can't.
bool PlayResource(int id, float gain) {
    struct Clip {
        int id;
        unsigned rate;
        std::vector<float> stereo, crushed;
    };
    static std::vector<Clip> cache;  // decoded on first use
    if (!id) return false;
    auto it = std::find_if(cache.begin(), cache.end(), [&](const Clip& c) { return c.id == id; });
    if (it == cache.end()) {
        Clip c{id, 0, {}, {}};
        c.stereo = LoadGameSound(id, c.rate);
        c.crushed = Bitcrush(c.stereo);
        cache.push_back(std::move(c));
        it = cache.end() - 1;
    }
    // Queen's laugh is crushed already.
    bool crush = RoboticSounds() && id != IDR_SND_QUEEN_BITCRUSHLAUGH;
    return PlayThemeClip(crush ? it->crushed : it->stereo, it->rate, gain);
}

}  // namespace

// The games play their music as a clip too: a clip that isn't a theme's sound is left alone, and the
// synthesized sound plays beside it instead.
bool PlayThemeClip(const std::vector<float>& stereo, unsigned rate, float gain) {
    ULONGLONG now = GetTickCount64();
    if (stereo.empty() || !rate || (sfx::ClipPosition() >= 0 && now > ourClipUntil)) return false;
    sfx::PlayClip(stereo, rate, gain);
    ourClipUntil = now + stereo.size() / 2 * 1000 / rate + 250;
    return true;
}

// The sounds are stored as FLAC, which holds the game's samples exactly in about two thirds of the
// packed bytes that PCM WAV needs.
std::vector<float> LoadGameSound(int id, unsigned& rate) {
    std::vector<float> out;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    const uint8_t* p = data ? (const uint8_t*)LockResource(data) : nullptr;
    auto decoder = p ? OpenFlacDecoder(p, SizeofResource(nullptr, res)) : nullptr;
    if (!decoder) return out;
    rate = decoder->Rate();
    out.resize(2 * (size_t)std::llround(decoder->Duration() * rate));
    out.resize(2 * decoder->Read(out.data(), out.size() / 2));
    return out;
}

bool RoboticSounds() { return s_robotic; }

void SetRoboticSounds(bool on) { s_robotic = on; }

void ToggleRoboticSounds() {
    s_robotic = !s_robotic;
    IniWrite(L"Window", L"RoboticSounds", s_robotic ? L"1" : L"0");
    if (s_robotic) MessageBlip();
}

// A status message in the DELTARUNE theme with robotic sounds, spoken in Queen's blip; at most one
// every second and a half, so a burst of messages doesn't chatter.
void MessageBlip() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    // A sound effect still playing (the one that came with the message) isn't cut off by it.
    if (!ThemeInfo(g.themeMode).gameSounds || !RoboticSounds() || !g.soundEffects || g.exclusive || now - last < 1500 ||
        (now < ourClipUntil && sfx::ClipPosition() >= 0))
        return;
    last = now;
    bool ducked = g.playing != kNone && !g.engine.Paused();
    double v = g.volumePercent / 100.0;
    PlayResource(IDR_SND_TXTQ, (float)(0.3 * v * v * (ducked ? 0.6 : 1)));
}

bool PlayGameSound(Sfx sound, float gain) { return PlayResource(SoundFor(sound), gain); }

}  // namespace app
