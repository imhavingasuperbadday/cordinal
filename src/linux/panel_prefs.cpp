// Preferences on Linux (File > Preferences, Ctrl+,), as the Windows build's Tweaks registry and window
// (app_tweaks.cpp) for the settings this build has: a page per area plus All settings, one search across
// them, and each setting with its help, its default and a reset. A change applies at once through the same
// function its menu item uses, and is saved. The command palette lists these settings too.
#include "panels_internal.h"

#include "discord.h"
#include "listenbrainz.h"
#include "metadata_fetch.h"
#include "musicbrainz.h"
#include "textcase.h"

#include <array>
#include <cstring>

namespace lx::panel {
namespace {

// The Windows build's Preferences (app_tweaks.cpp) for the settings this build has: a page per area
// plus All settings, one search across them, and each setting with its help, its default and a reset.
// A change applies at once through the same function its menu item uses, and is saved.
enum class PrefType { Bool, Float, Choice };
struct Pref {
    const char* key;  // in settings.ini; permanent, as renaming one would orphan saved values
    const char* page;
    const char* label;
    const char* help;
    PrefType type;
    float min, max, def, step;
    double (*get)();
    void (*set)(double);
    const char* const* choices = nullptr;
    const char* unit = nullptr;
};

const char* const kRgModes[] = {"Album", "Track", "By playback order"};
const char* const kOffOn[] = {"Off", "On"};
const char* const kDiscordNames[] = {"Off", "Song", "Album"};
const char* const kLoopNames[] = {"Ignore loop points", "Play the loop once, then fade", "Play the loop twice, then fade",
                                  "Play the loop three times, then fade"};
const char* const kReverbNames[] = {"Off", "Room", "Hall", "Plate"};
const char* const kSavingNames[] = {"Ask before saving", "Save automatically", "Manual only (don't ask)"};
const char* const kNewMusicNames[] = {"Add automatically", "Review in Inbox"};
const char* const kBandWaveSizes[] = {"Use layout size", "Slim (26 px)", "Medium (44 px)", "Tall (72 px)",
                                    "Extra tall (108 px)", "Largest (144 px)"};
static_assert(std::size(kBandWaveSizes) == std::size(ui::kBandWaveHeights));
// The seek bar's look: a plain progress bar first, then the SeekStyle values (peaks.h) one up.
const char* const kSeekBarNames[] = {"Progress bar", "Waveform", "Moodbar", "Mood waveform"};
// Waveform reflection's shapes. Mirrored is wave_reflection on; with it off, wave_full_height (default on)
// picks Full height over Upper half, so settings from before the choice open in Full height.
const char* const kWaveShapeNames[] = {"Upper half", "Mirrored", "Full height"};
const char* const kSeekShowsNames[] = {"Whole song", "Live audio", "Live audio with progress"};
const char* const kMoodColorNames[] = {"Theme", "Vivid", "Spectrum", "Warm", "Cool", "Colour-blind friendly",
                                       "Single colour"};
const char* const kMoodWaveColorNames[] = {"Same as moodbar", "Theme", "Vivid", "Spectrum", "Warm", "Cool",
                                           "Colour-blind friendly", "Single colour"};
const char* const kWaveColorNames[] = {"Theme", "Red", "Orange", "Yellow", "Green", "Teal", "Blue", "Purple", "Pink",
                                       "Grey"};
const char* const kWaveRestNames[] = {"Theme grey", "Faded waveform colour"};
const char* const kAlbumColourNames[] = {"Off", "Player background", "Whole interface"};
const char* const kAlbumStrengthNames[] = {"Subtle", "Normal", "Strong", "Bold"};
static_assert(std::size(kAlbumColourNames) == (size_t)AlbumColours::Interface + 1 &&
              std::size(kAlbumStrengthNames) == kAlbumStrengths);
static_assert(std::size(kSeekBarNames) == kSeekStyles + 1 && std::size(kMoodColorNames) == kMoodColorSchemes &&
              std::size(kMoodWaveColorNames) == kMoodColorSchemes + 1 &&
              std::size(kWaveColorNames) == kWaveColorChoices && std::size(kWaveRestNames) == kWaveRestChoices);
const char* const kBitcrushBitNames[] = {"2 bits", "3 bits", "4 bits", "5 bits", "6 bits", "7 bits", "8 bits",
                                         "9 bits", "10 bits", "11 bits", "12 bits", "13 bits", "14 bits", "15 bits",
                                         "16 bits"};
const char* const kBitcrushRateNames[] = {"2 kHz", "4 kHz", "6 kHz", "8 kHz", "11 kHz", "16 kHz", "22 kHz", "32 kHz",
                                          "48 kHz"};
static_assert(std::size(kBitcrushBitNames) == kBitcrushMaxBits - kBitcrushMinBits + 1 &&
              std::size(kBitcrushRateNames) == std::size(kBitcrushRates));
const char* const kUndoNames[] = {"1 day", "3 days", "1 week", "7 weeks"};
const char* const kCrossfadeNames[] = {"Off", "1 second", "2 seconds", "3 seconds", "5 seconds", "8 seconds"};
const char* const kVolumeCurveNames[] = {"Natural", "Linear", "Cubic", "Decibels (60 dB range)"};
static_assert(std::size(kCrossfadeNames) == std::size(kCrossfadeSeconds) && std::size(kVolumeCurveNames) == kVolumeCurves);
const char* const kCoverSourceNames[] = {"Deezer", "iTunes", "MusicBrainz"};
const char* const kCoverSaveNames[] = {"Save as cover.jpg in the album folder", "Embed in the files' tags",
                                      "Show in cordinal only"};
static_assert(std::size(kCoverSourceNames) == online::kCoverSources);
static_assert(std::size(kReverbNames) == (size_t)ReverbPreset::Count);

Settings& Cfg() { return g.settings; }
void Saved() {
    SaveSettings();
    g.redraw = true;
}

const Pref kPrefs[] = {
    {"resume_on", "Playback", "Resume where you left off",
     "When cordinal starts, offer to continue the track that was playing when it closed.", PrefType::Bool,
     0, 1, 1, 1, [] { return (double)Cfg().resumeOn; }, [](double v) { SetResumeOn(v != 0); }},
    {"resume_in_song", "Playback", "Reopen in the last song",
     "Cordinal starts with the last track loaded and paused at the point it stopped, instead of waiting for Continue.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)Cfg().resumeInSong; },
     [](double v) { Cfg().resumeInSong = v != 0; ReopenLastTrack(); Saved(); }},
    {"resume_play", "Playback", "Start it playing on open",
     "The track reopened from the last session starts playing as soon as cordinal opens, instead of waiting paused.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)Cfg().resumePlay; },
     [](double v) { Cfg().resumePlay = v != 0; ReopenLastTrack(); Saved(); }},
    {"crossfade", "Playback", "Crossfade between tracks",
     "When one track runs into the next on its own, the end of the first fades out as the next fades in. "
     "A track you start yourself, a radio stream and a section loop still change at once.",
     PrefType::Choice, 0, (float)std::size(kCrossfadeNames) - 1, 0, 1, [] { return (double)Cfg().crossfade; },
     [](double v) { Cfg().crossfade = (int)v; ApplyEffects(); RefreshNext(); Saved(); }, kCrossfadeNames},
    {"crossfade_albums", "Playback", "Crossfade within an album",
     "Off keeps the tracks of one album gapless, so live and continuous albums flow as recorded; the crossfade "
     "then only joins different albums.", PrefType::Bool, 0, 1, 0, 1, [] { return (double)Cfg().crossfadeAlbums; },
     [](double v) { Cfg().crossfadeAlbums = v != 0; RefreshNext(); Saved(); }},
    {"loops", "Playback", "Game music loops", "How often a game soundtrack rip with loop points plays its loop before fading out.",
     PrefType::Choice, 0, 3, 2, 1, [] { return (double)Cfg().loops; },
     [](double v) { Cfg().loops = (int)v; ApplyEffects(); Saved(); }, kLoopNames},
    {"skip_aware_shuffle", "Playback", "Skip-aware shuffle",
     "Shuffle favors tracks you haven't repeatedly skipped in their first 30 seconds. A skip while flipping through "
     "songs counts for less, and one just before you stop doesn't count.", PrefType::Bool,
     0, 1, 0, 1, [] { return (double)Cfg().skipAwareShuffle; }, [](double v) { SetSkipAwareShuffle(v != 0); }},
    {"radio", "Playback", "Keep playing like this",
     "Keeps choosing from the playing playlist, favoring what you tend to play after the current track.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)Cfg().radio; }, [](double v) { SetRadio(v != 0); }},
    {"taste", "Playback", "Choose by taste",
     "Track shuffle and Keep playing like this follow what you finish, rate, replay and come back to. Skips are judged by "
     "their moment, and artists by how often you finish them, not how many of their songs you have.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)Cfg().taste; }, [](double v) { SetTaste(v != 0); }},
    {"exclusive", "Output", "Exclusive output",
     "Each track plays at its own rate, bit-perfect at 100% volume with the EQ and ReplayGain off.", PrefType::Bool, 0, 1, 0,
     1, [] { return (double)Cfg().exclusive; }, [](double v) { SetExclusive(v != 0); }},
    {"software_volume", "Output", "Volume control in cordinal",
     "Off sends the music at full level and leaves the volume to the system or your amplifier; cordinal's slider "
     "rests and only mute still works. With Exclusive output, that keeps playback bit-perfect at any level.",
     PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().softwareVolume; },
     [](double v) { Cfg().softwareVolume = v != 0; SetVolume(Cfg().volume); Saved(); }},
    {"volume_curve", "Output", "Volume curve",
     "How the slider's travel turns into loudness. Natural suits most listening; Linear is the plain level, "
     "which bunches loudness at the bottom; Cubic gives finer steps when quiet; Decibels spreads 60 dB evenly.",
     PrefType::Choice, 0, kVolumeCurves - 1, 0, 1, [] { return (double)Cfg().volumeCurve; },
     [](double v) { Cfg().volumeCurve = (int)v; SetVolume(Cfg().volume); Saved(); }, kVolumeCurveNames},
    {"rg_enabled", "ReplayGain", "ReplayGain", "Plays every track at a similar loudness, using measured or tagged ReplayGain values.",
     PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().rg.on; }, [](double v) { SetReplayGain(v != 0); }},
    {"rg_mode", "ReplayGain", "Level by",
     "Album preserves album relationships; track levels songs individually. Playback order uses track for track shuffle.",
     PrefType::Choice, 0, 2, 0, 1, [] { return (double)Cfg().rg.mode; },
     [](double v) { Cfg().rg.mode = (ReplayGainMode)(int)v; RefreshGains(); Saved(); }, kRgModes},
    {"rg_keep", "ReplayGain", "Loud songs keep their loudness",
     "0 evens every track out; higher values turn loud songs down only part of the way.", PrefType::Float, 0, 0.9f, 0.4f,
     0.05f, [] { return (double)Cfg().rg.keep; }, [](double v) { Cfg().rg.keep = (float)v; RefreshGains(); Saved(); }},
    {"rg_preamp", "ReplayGain", "Preamp", "Added to every track's ReplayGain.", PrefType::Float, -24, 24, 0, 0.5f,
     [] { return (double)Cfg().rg.preamp; }, [](double v) { Cfg().rg.preamp = (float)v; RefreshGains(); Saved(); }, nullptr, "dB"},
    {"rg_preamp_noinfo", "ReplayGain", "Gain for unmeasured tracks",
     "Used for tracks with no ReplayGain yet, limited so their peaks don't clip.", PrefType::Float, -24, 24, 0, 0.5f,
     [] { return (double)Cfg().rg.preampNoInfo; }, [](double v) { Cfg().rg.preampNoInfo = (float)v; RefreshGains(); Saved(); },
     nullptr, "dB"},
    {"rg_autoscan", "ReplayGain", "Measure new tracks automatically",
     "Measures the loudness of new tracks in the background, giving way to playback and to your own scans.", PrefType::Bool,
     0, 1, 1, 1, [] { return (double)Cfg().rgAutoScan; }, [](double v) { Cfg().rgAutoScan = v != 0; Saved(); }},
    {"rg_estimate", "ReplayGain", "Estimate unmeasured tracks as they start",
     "Guesses the loudness of a track with no ReplayGain from its first moments while it plays.", PrefType::Bool, 0, 1, 1, 1,
     [] { return (double)Cfg().rgEstimate; }, [](double v) { Cfg().rgEstimate = v != 0; Saved(); }},
    {"reverb", "Effects", "Reverb", "A room around the music.", PrefType::Choice, 0, 3, 0, 1,
     [] { return (double)Cfg().reverb.preset; },
     [](double v) { Cfg().reverb.preset = (ReverbPreset)(int)v; ApplyEffects(); Saved(); }, kReverbNames},
    {"reverb_amount", "Effects", "Reverb amount", "How much of the room is mixed in, from 0 (none) to 1 (all of it).",
     PrefType::Float, 0, 1, 0.4f, 0.05f, [] { return (double)Cfg().reverb.mix; },
     [](double v) { Cfg().reverb.mix = (float)v; ApplyEffects(); Saved(); }},
    {"loudness_compensation", "Effects", "Loudness compensation at low volume",
     "Adds bass and treble as the volume goes down, so quiet listening doesn't sound thin.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().loudnessComp; }, [](double v) { Cfg().loudnessComp = v != 0; ApplyEffects(); Saved(); }},
    {"night_mode", "Effects", "Night mode", "Slowly lifts quiet passages and softens loud ones, for listening at night.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)Cfg().nightMode; },
     [](double v) { Cfg().nightMode = v != 0; ApplyEffects(); Saved(); }},
    {"bitcrush", "Effects", "Bitcrush",
     "Lo-fi crunch: fewer levels and a lower sample rate, like an old sampler or game console.", PrefType::Bool, 0, 1,
     0, 1, [] { return (double)Cfg().bitcrush.enabled; },
     [](double v) { Cfg().bitcrush.enabled = v != 0; ApplyEffects(); Saved(); }},
    {"bitcrush_bits", "Effects", "Bitcrush depth",
     "How many bits each sample keeps. Fewer is harsher and buzzier; at 4 or fewer, quiet passages drop out.",
     PrefType::Choice, 0, (float)std::size(kBitcrushBitNames) - 1, 8 - kBitcrushMinBits, 1,
     [] { return (double)(Cfg().bitcrush.bits - kBitcrushMinBits); },
     [](double v) { Cfg().bitcrush.bits = (int)v + kBitcrushMinBits; ApplyEffects(); Saved(); }, kBitcrushBitNames},
    {"bitcrush_rate", "Effects", "Bitcrush sample rate",
     "The rate the sound is held at. Lower rates lose treble and add metallic aliasing.", PrefType::Choice, 0,
     (float)std::size(kBitcrushRateNames) - 1, kBitcrushDefaultRate, 1,
     [] { return (double)BitcrushRateIndex(Cfg().bitcrush.rate); },
     [](double v) {
         Cfg().bitcrush.rate = kBitcrushRates[std::clamp((int)v, 0, (int)std::size(kBitcrushRates) - 1)];
         ApplyEffects();
         Saved();
     }, kBitcrushRateNames},
    {"bitcrush_mix", "Effects", "Bitcrush mix",
     "How much of the crushed sound replaces the clean one, from 0 (none) to 1 (all of it).", PrefType::Float, 0, 1, 1,
     0.05f, [] { return (double)Cfg().bitcrush.mix; },
     [](double v) { Cfg().bitcrush.mix = (float)v; ApplyEffects(); Saved(); }},
    // Seek bar and Seek bar shows each stand for two settings earlier versions offered as separate
    // switches; settings.ini keeps those keys as they were, as on Windows (app_tweaks.cpp).
    {"seek_bar", "View", "Seek bar",
     "How the seek bar looks. Moodbar colours each moment by its sound: one way for bass, the other for "
     "treble, brighter when louder. Mood waveform keeps the waveform's shape in those colours.",
     PrefType::Choice, 0, 3, 1, 1, [] { return Cfg().waveform ? (double)Cfg().seekStyle + 1 : 0.0; },
     [](double v) { Cfg().waveform = v != 0; if (v != 0) Cfg().seekStyle = (int)v - 1; Saved(); }, kSeekBarNames},
    {"seek_shows", "View", "Seek bar shows",
     "The whole song, or a moving picture of the audio being heard; seeking still works either way, and Radio "
     "always shows live audio. With progress, what has played is lit and the rest dimmed.",
     PrefType::Choice, 0, 2, 0, 1, [] { return !Cfg().liveWaveform ? 0.0 : Cfg().fullLiveWaveform ? 1.0 : 2.0; },
     [](double v) { Cfg().liveWaveform = v != 0; if (v != 0) Cfg().fullLiveWaveform = v == 1; Saved(); },
     kSeekShowsNames},
    {"wave_60fps", "View", "60 frames a second",
     "Draws the live waveform and the seek bar's glow 60 times a second instead of the default 30, for "
     "smoother motion. Uses a little more CPU.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().wave60Fps; }, [](double v) { Cfg().wave60Fps = v != 0; Saved(); }},
    {"seek_glow", "View", "Seek bar glow",
     "A soft halo of the seek bar's colours around it that pulses with the beat, brighter where the song is loud. "
     "Every look, whole song or live.", PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().seekGlow; },
     [](double v) { Cfg().seekGlow = v != 0; Saved(); }},
    {"pause_fade", "View", "Fade the waveform when pausing",
     "The waveform's colour fades smoothly between playing and paused, instead of switching at once. Paused "
     "turns amber, or the text colour when your waveform colour is itself amber or yellow.",
     PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().pauseFade; },
     [](double v) { Cfg().pauseFade = v != 0; Saved(); }},
    {"live_fountain", "View", "Live audio for Dark Fountain",
     "In the DELTARUNE theme, Dark Fountain follows the audio being heard, including Radio. "
     "Song seeking still works.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().liveFountain; }, [](double v) { Cfg().liveFountain = v != 0; Saved(); }},
    {"list_wind", "View", "Wind behind the track list",
     "Soft ribbons drift across the right of the track list while music plays, curling, swelling and "
     "gusting with it. Bass makes them wide and slow, treble short and quick.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().listWind; }, [](double v) { Cfg().listWind = v != 0; Saved(); }},
    {"band_wave_size", "View", "Waveform height in the top or bottom bar",
     "Sets the waveform's height in the normal player bar. Other player modes keep their own sizes.",
     PrefType::Choice, 0, 5, 0, 1, [] { return (double)Cfg().bandWaveSize; },
     [](double v) { Cfg().bandWaveSize = (int)v; Saved(); }, kBandWaveSizes},
    {"album_colors", "View", "Album colours",
     "How much of the window takes the playing album's colour: none, the player's background and an open "
     "album's panel, or the whole interface, its accent and selection too. Light and Dark only; text stays readable.",
     PrefType::Choice, 0, (float)AlbumColours::Interface, (float)AlbumColours::Player, 1,
     [] { return (double)AlbumColourConfig().mode; },
     [](double v) { AlbumColourConfig().mode = (AlbumColours)std::clamp((int)v, 0, (int)AlbumColours::Interface);
                    g.redraw = true; Saved(); }, kAlbumColourNames},
    {"album_color_strength", "View", "Album colour strength",
     "How strongly the album's colour shows, from Subtle to Bold. A colour that would make text hard to read "
     "is held back.", PrefType::Choice, 0, kAlbumStrengths - 1, 1, 1, [] { return (double)AlbumColourConfig().strength; },
     [](double v) { AlbumColourConfig().strength = std::clamp((int)v, 0, kAlbumStrengths - 1); g.redraw = true; Saved(); },
     kAlbumStrengthNames},
    {"wave_color", "View", "Waveform colour",
     "The waveform's played colour, kept bright enough to read on the player. The moodbar's Theme, Vivid and "
     "Single colour schemes turn from it too. Paused and warnings keep their own colours.",
     PrefType::Choice, 0, kWaveColorChoices - 1, 0, 1, [] { return (double)Cfg().waveColor; },
     [](double v) { Cfg().waveColor = (int)v; Saved(); }, kWaveColorNames},
    {"wave_rest_color", "View", "Waveform unplayed part",
     "What is still to play: the theme's grey, or the waveform's own colour faded toward the background.",
     PrefType::Choice, 0, kWaveRestChoices - 1, 0, 1, [] { return (double)Cfg().waveRestColor; },
     [](double v) { Cfg().waveRestColor = (int)v; Saved(); }, kWaveRestNames},
    {"mood_colors", "View", "Moodbar colours",
     "The moodbar's colours: turned from the waveform colour, the whole colour wheel, red for bass, green "
     "for mids and blue for treble, warm or cool shades, blue to orange (easy to tell apart with colour "
     "blindness), or one colour that stands out more for treble.",
     PrefType::Choice, 0, kMoodColorSchemes - 1, 0, 1, [] { return (double)Cfg().moodColors; },
     [](double v) { Cfg().moodColors = (int)v; Saved(); }, kMoodColorNames},
    {"mood_wave_colors", "View", "Mood waveform colours",
     "The Mood waveform's colours: the moodbar's, or a scheme of its own.", PrefType::Choice, 0, kMoodColorSchemes, 0,
     1, [] { return (double)Cfg().moodWaveColors; }, [](double v) { Cfg().moodWaveColors = (int)v; Saved(); },
     kMoodWaveColorNames},
    {"smooth_seek", "View", "Smooth seek bar",
     "Rounds off the whole song's outline and blends its moodbar colours into each other. "
     "The live waveform keeps its detail.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)Cfg().smoothSeek; },
     [](double v) { Cfg().smoothSeek = v != 0; Saved(); }},
    {"wave_reflection", "View", "Waveform reflection",
     "Mirrored reflects the seek bar below its center line, dimmer. Upper half stops at the center line; "
     "Full height mirrors the waveforms as bright below as above (the Moodbar stripe keeps the upper "
     "half).",
     PrefType::Choice, 0, 2, 1, 1, [] { return Cfg().waveReflection ? 1.0 : Cfg().waveFullHeight ? 2.0 : 0.0; },
     [](double v) { Cfg().waveReflection = v == 1; if (v != 1) Cfg().waveFullHeight = v == 2; Saved(); },
     kWaveShapeNames},
    {"scrobbling_paused", "Scrobbling", "Pause scrobbling",
     "Stops collecting and sending listens to Last.fm, ListenBrainz and Libre.fm, keeping connections and queued listens.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)listenbrainz::Paused(); },
     [](double v) { listenbrainz::SetPaused(v != 0); Saved(); }},
    {"scrobbling_now_playing", "Scrobbling", "Send now-playing updates",
     "Shows the current song on connected services. Turning this off still submits completed listens.",
     PrefType::Bool, 0, 1, 1, 1, [] { return (double)listenbrainz::SendNowPlaying(); },
     [](double v) { listenbrainz::SetSendNowPlaying(v != 0); Saved(); }},
    {"discord_status", "Scrobbling", "Discord status",
     "While the Discord app runs on this computer, your status shows the song playing or the album, with how far "
     "along it is. Nothing is shown while paused. Discord shows it only while its own Activity Privacy setting "
     "shares your activity. The album cover comes from the cover sources turned on under Online.",
     PrefType::Choice, 0, discord::kShowChoices - 1, 0, 1, [] { return (double)discord::Mode(); },
     [](double v) { discord::SetMode((discord::Show)v); Saved(); }, kDiscordNames},
    {"musicbrainz_lookup", "Library", "Look up missing album editions online",
     "Asks MusicBrainz for a release-group id when two local albums share an artist and base title and their tags carry none. "
     "Results are cached; browsing and playback never wait for or require the network.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().musicBrainzLookup; },
     [](double v) { Cfg().musicBrainzLookup = v != 0; musicbrainz::SetEnabled(v != 0); Saved(); }},
    // Online sources, as on Windows: each says exactly what it sends, and all are off until chosen.
    {"online_covers_deezer", "Online", "Covers from Deezer",
     "Sends the album's artist and title to api.deezer.com "
     "when you look for a cover, or for an album cover in the Discord status.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)online::Config().covers[0]; }, [](double v) { online::Config().covers[0] = v != 0; Saved(); }},
    {"online_covers_itunes", "Online", "Covers from iTunes",
     "Sends the album's artist and title to itunes.apple.com "
     "when you look for a cover, or for an album cover in the Discord status.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)online::Config().covers[1]; }, [](double v) { online::Config().covers[1] = v != 0; Saved(); }},
    {"online_covers_musicbrainz", "Online", "Covers from MusicBrainz",
     "Sends the album's artist and title to musicbrainz.org, then the release it finds to coverartarchive.org, "
     "when you look for a cover or for one in the Discord status.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)online::Config().covers[2]; },
     [](double v) { online::Config().covers[2] = v != 0; Saved(); }},
    {"online_cover_first", "Online", "Preferred cover source",
     "Its covers are listed first, and it wins when two covers are about as good.", PrefType::Choice, 0, 2, 0, 1,
     [] { return (double)online::Config().coverFirst; }, [](double v) { online::Config().coverFirst = (int)v; Saved(); },
     kCoverSourceNames},
    {"online_cover_save", "Online", "Save chosen covers",
     "Shown in cordinal only, or as a folder file, no audio file changes. Embedding rewrites each track's tags; Undo last tag edit takes it back.",
     PrefType::Choice, 0, 2, 2, 1, [] { return (double)online::Config().coverSave; },
     [](double v) { online::Config().coverSave = (int)v; Saved(); }, kCoverSaveNames},
    {"online_cover_overwrite", "Online", "Replace an existing cover file",
     "When off, an album folder that already has cover.jpg, folder.jpg or front.jpg keeps it.", PrefType::Bool, 0, 1, 0,
     1, [] { return (double)online::Config().coverOverwrite; },
     [](double v) { online::Config().coverOverwrite = v != 0; Saved(); }},
    {"online_lyrics_lrclib", "Online", "Lyrics from LrcLib",
     "Sends the song's artist, title, album and length to lrclib.net when it has no lyrics of its own.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)online::Config().lyrics[0]; },
     [](double v) { online::Config().lyrics[0] = v != 0; LyricsSourcesChanged(); Saved(); }},
    {"online_lyrics_ovh", "Online", "Lyrics from lyrics.ovh",
     "Sends the song's artist and title to api.lyrics.ovh when it has no lyrics of its own and LrcLib had none.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)online::Config().lyrics[1]; },
     [](double v) { online::Config().lyrics[1] = v != 0; LyricsSourcesChanged(); Saved(); }},
    {"online_artist_info", "Online", "Artist info from MusicBrainz and Wikipedia",
     "Sends the artist's name to musicbrainz.org, which also lists their albums, then asks wikidata.org and "
     "en.wikipedia.org for the article it names.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)online::Config().artistInfo; },
     [](double v) { online::Config().artistInfo = v != 0; Saved(); }},
    {"group_albums", "View", "Group tracks by album", "The track list shows a header with the cover above each album's tracks.",
     PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().groupAlbums; },
     [](double v) { Cfg().groupAlbums = v != 0; RebuildRows(); Saved(); }},
    {"now_playing_album", "View", "Now Playing shows the playing album",
     "While a song with an album plays, the Now Playing tab lists just that album. Typing a search looks through the "
     "whole list again; songs without an album always show the whole list.",
     PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().npAlbumOnly; },
     [](double v) { Cfg().npAlbumOnly = v != 0; RebuildRows(); Saved(); }},
    {"group_auto_tabs", "View", "One tab for auto-playlists",
     "Auto-playlists such as Recently added and Top rated share one tab; click it again to choose another. Off gives "
     "each its own tab.", PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().groupAutoTabs; },
     [](double v) { Cfg().groupAutoTabs = v != 0; g.redraw = true; Saved(); }},
    {"points_chip", "View", "Show level and points",
     "Shows your rank, level and points beside the now-playing lines, and announces a new level.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().layout.points; }, [](double v) { Cfg().layout.points = v != 0; Saved(); }},
    {"preference_saving", "Startup", "Preference saving",
     "Whether a new theme, library view or browser becomes how cordinal opens: asked about once it settles, saved as you go, or only "
     "through Startup defaults.", PrefType::Choice, 0, 2, 0, 1, [] { return (double)(int)Cfg().preferenceSaving; },
     [](double v) { SetPreferenceSaving((PreferenceSaving)(int)v); }, kSavingNames},
    {"time_remaining", "Playback", "Show the time remaining",
     "The time beside the seek bar counts down to the end of the track instead of showing its length.", PrefType::Bool, 0, 1, 1, 1,
     [] { return (double)Cfg().timeRemaining; }, [](double v) { Cfg().timeRemaining = v != 0; Saved(); }},
    {"smooth_scroll", "View", "Smooth scrolling",
     "The library eases toward where the mouse wheel or Page Up and Page Down take it, instead of "
     "jumping. Scrollbar drags and keyboard focus still move at once.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)Cfg().smoothScroll; },
     [](double v) { Cfg().smoothScroll = v != 0; Saved(); }},
    {"always_on_top", "View", "Always on top", "Keeps cordinal's window above other windows.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().alwaysOnTop; }, [](double v) { Cfg().alwaysOnTop = v != 0; SDL_SetWindowAlwaysOnTop(g.window, v != 0); Saved(); }},
    {"gpu_drawing", "View", "Draw with the graphics card",
     "Draws the window with the graphics card instead of the processor. Off uses less memory; turn it on if scrolling is slow on a "
     "large, high-resolution screen. Changes take effect immediately.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().gpuDrawing; }, [](double v) { SetGpuDrawing(v != 0); Saved(); }},
    {"sound_effects", "View", "Sound effects",
     "Small, quiet sounds for things that otherwise happen silently, such as saving tags or dropping files.", PrefType::Bool, 0, 1,
     0, 1, [] { return (double)Cfg().soundEffects; }, [](double v) { Cfg().soundEffects = v != 0; Saved(); }},
    {"robotic_sounds", "View", "Robotic sounds in the DELTARUNE theme",
     "The theme's sounds come bitcrushed, in Chapter 2's Cyber World voice, and messages are spoken in Queen's blip.",
     PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().roboticSounds; },
     [](double v) { Cfg().roboticSounds = v != 0; Saved(); if (v != 0) MessageBlip(); }},
    {nullptr, "Startup", "Start on login", "Opens cordinal when you log in (an entry in ~/.config/autostart).", PrefType::Bool,
     0, 1, 0, 1, [] { return (double)StartsOnLogin(); }, [](double v) { if ((v != 0) != StartsOnLogin()) ToggleStartOnLogin(); }},
    {"inbox_enabled", "Library", "New music from library folders",
     "Add new music automatically, or review it in the Inbox first. Existing Inbox tracks stay there.",
     PrefType::Choice, 0, 1, 0, 1, [] { return (double)Cfg().inboxEnabled; },
     [](double v) {
         Cfg().inboxEnabled = v != 0;
         Saved();
         ShowMessage(Cfg().inboxEnabled ? L"New music will wait in the Inbox." : L"New music will be added to the library.");
     }, kNewMusicNames},
    {"cover_gap", "Library", "Album cover spacing",
     "The least space between covers in the Albums and Artists views, in pixels. A row spreads out to fill the "
     "width, up to twice this.", PrefType::Float, 4, 48, 16, 2, [] { return (double)Cfg().coverGap; },
     [](double v) { Cfg().coverGap = std::clamp((int)std::lround(v), 4, 48); CanvasRelayout(); Saved(); }, nullptr, "px"},
    {"web_start", "Library", "Start the web server with cordinal",
     "Turns on the web server whenever cordinal starts, so a paired phone or browser can play your library.", PrefType::Bool,
     0, 1, 0, 1, [] { return (double)Cfg().webStart; }, [](double v) { Cfg().webStart = v != 0; Saved(); }},
    {"convert_recycle", "Conversion", "Send converted originals to the Recycle Bin",
     "After converting to .cord, the original files go to the Recycle Bin instead of staying where they are.", PrefType::Bool,
     0, 1, 1, 1, [] { return (double)Cfg().convertRecycle; }, [](double v) { Cfg().convertRecycle = v != 0; Saved(); }},
    {"convert_show_originals", "Conversion", "Show kept originals in the library",
     "Originals kept after a conversion appear in the library beside their .cord copies.", PrefType::Bool,
     0, 1, 0, 1, [] { return (double)Cfg().convertShowOriginals; }, [](double v) { SetConvertShowOriginals(v != 0); Saved(); }},
    {"convert_undo_days", "Conversion", "Conversions can be undone for",
     "How long cordinal remembers a conversion so it can put the originals back.", PrefType::Choice,
     0, 3, 3, 1, [] { return (double)Cfg().convertMemory; }, [](double v) { SetConvertMemory((unsigned)v); Saved(); }, kUndoNames},
};

struct PrefLink {
    const char* label;
    void (*open)();
    bool (*enabled)() = nullptr;
    bool (*shown)() = nullptr;
    bool action = false;  // a scan keeps Preferences open, rather than replacing it with another panel
};
struct PrefPage {
    const char* name;   // null for All settings
    const char* about;  // the line under the page's title, as on Windows
    PrefLink links[4];
    const char* group = nullptr;  // the caption above the page in the list, where a group starts
};
// The Windows build's pages, captions and lines (app_tweaks.cpp, kPages): sound, look, the collection,
// and the rest. Layout has no settings of its own here; its page links to the windows that hold them.
const PrefPage kPrefPages[] = {
    {nullptr, "Every setting on one page, under the page it belongs to.", {}},
    {"Playback", "Resuming, how tracks change, shuffle and radio, and the time shown.", {}, "Sound"},
    {"Output", "Exclusive output and the volume control.",
     {{"Output device...", OpenOutputDevice}, {"Headphones and devices...", OpenHeadphones}}},
    {"ReplayGain", "Even loudness from track to track, and measuring it.",
     {{"Scan selection", [] { ScanReplayGain(SelectedTracks()); },
       [] { return (!g.rgRunning || g.rgAuto) &&
                   std::any_of(g.selected.begin(), g.selected.end(), [](char value) { return value != 0; }); }, nullptr, true},
      {"Scan playlist", [] { ScanReplayGain(PlaylistTracks(Active())); },
       [] { return (!g.rgRunning || g.rgAuto) && !g.rows.empty(); }, nullptr, true},
      {"Scan library", [] { ScanReplayGain(replaygain::LibraryTracks()); },
       [] { return (!g.rgRunning || g.rgAuto) && replaygain::HasLibraryTracks(); }, nullptr, true},
      {"Cancel scan", CancelReplayGain, nullptr, [] { return g.rgRunning && !g.rgAuto; }, true}}},
    {"Effects", "Reverb, volume shaping and bitcrush, after the equalizer.", {{"Equalizer...", OpenEqualizer}}},
    {"View", "The seek bar and its colours, and how the window behaves.", {{"Title formatting...", OpenFormat}}, "Look"},
    {"Layout", "Where the player sits and which parts of the normal window show.",
     {{"Customize layout...", OpenLayout}, {"Startup defaults...", OpenStartupDefaults}}},
    {"Library", "New music, sizes in the library views, and the web server.",
     {{"Library folders...", OpenLibraryFolders}, {"Web server...", OpenWebServer}}, "Collection"},
    {"Conversion", "Converting to .cord, and putting the originals back.", {}},
    {"Online", "Covers, lyrics and artist info from the web. Each says what it sends, and all start off.",
     {{"Fetch missing covers...", FetchMissingCovers, [] { return !CoverFetchBusy(); }}}},
    {"Scrobbling", "Listens sent to connected services, and your Discord status.", {{"Scrobbling...", OpenListenBrainz}}},
    {"Startup", "How cordinal opens, and what it keeps for next time.", {{"Startup defaults...", OpenStartupDefaults}}, "General"},
};

// Headings inside a page, each above the setting it names and the ones after it on that page.
struct PrefHeading {
    const char* key;
    const char* title;
};
const PrefHeading kPrefHeadings[] = {
    {"resume_on", "Resuming"}, {"crossfade", "Track changes"}, {"skip_aware_shuffle", "Shuffle and radio"},
    {"time_remaining", "Time"}, {"exclusive", "Device"}, {"software_volume", "Volume"},
    {"rg_enabled", "Levelling"}, {"rg_autoscan", "Measuring"},
    {"reverb", "Reverb"}, {"loudness_compensation", "Volume shaping"}, {"bitcrush", "Bitcrush"},
    {"seek_bar", "Seek bar"}, {"album_colors", "Colours"}, {"smooth_seek", "Shape"}, {"group_albums", "Window"},
    {"sound_effects", "Sounds"}, {"musicbrainz_lookup", "Adding music"}, {"cover_gap", "Sizes"},
    {"web_start", "Outside cordinal"},
    {"online_covers_deezer", "Covers"}, {"online_lyrics_lrclib", "Lyrics"}, {"online_artist_info", "Artist info"},
};

int FindPrefPage(const Pref& t) {
    for (int i = 1; i < (int)std::size(kPrefPages); ++i)
        if (!strcmp(kPrefPages[i].name, t.page)) return i;
    return 0;
}

// The heading a setting sits under on its page: the last one at or before it, so a heading still
// shows when the setting it names is hidden.
const char* FindPrefHeading(int pref) {
    const char* heading = nullptr;
    for (int i = 0; i <= pref; ++i) {
        if (strcmp(kPrefs[i].page, kPrefs[pref].page) || !kPrefs[i].key) continue;
        for (const PrefHeading& h : kPrefHeadings)
            if (!strcmp(h.key, kPrefs[i].key)) heading = h.title;
    }
    return heading;
}

// Where each setting sits, found once: the tables never change.
struct PrefPlace {
    int page = 0;
    const char* heading = nullptr;
};
const PrefPlace& PlaceOf(int pref) {
    static const auto places = [] {
        std::array<PrefPlace, std::size(kPrefs)> a{};
        for (int i = 0; i < (int)std::size(kPrefs); ++i) a[i] = {FindPrefPage(kPrefs[i]), FindPrefHeading(i)};
        return a;
    }();
    return places[pref];
}

// Each setting's label and help, wrapped to the sheet's text width as Windows wraps its static
// controls (DT_WORDBREAK). Wrapping measures every word, which for All settings is thousands of
// measurements, so the lines are kept until the width, the font or the search changes.
struct WrapCache {
    float width = -1, sample = -1;
    bool searching = false;
    std::vector<std::vector<std::string>> label, help;
    std::vector<char> done;
} g_wrap;

// A typed value as it reads best: 16, 0.4, -2.5 (app_tweaks.cpp, NumberText).
std::string NumberText(double value) {
    char text[32];
    snprintf(text, sizeof(text), "%.2f", value);
    std::string s = text;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

std::string PrefText(const Pref& t, double v);

// Windows puts the default after the help, and with a search the page the setting is on.
std::string HelpText(int pref, bool searching) {
    const Pref& t = kPrefs[pref];
    std::string def = t.type == PrefType::Float ? NumberText(t.def) + (t.unit ? std::string(" ") + t.unit : "") : PrefText(t, t.def);
    std::string help = std::string(t.help) + " Default: " + def + ".";
    if (searching) help += std::string(" (") + t.page + ")";
    return help;
}

void Wrapped(int pref, float width, bool searching, const std::vector<std::string>** label, const std::vector<std::string>** help) {
    const float sample = TextWidth(Font::Normal, "The quick brown fox jumps over the lazy dog");
    if (g_wrap.width != width || g_wrap.sample != sample || g_wrap.searching != searching) {
        g_wrap.width = width;
        g_wrap.sample = sample;
        g_wrap.searching = searching;
        g_wrap.label.assign(std::size(kPrefs), {});
        g_wrap.help.assign(std::size(kPrefs), {});
        g_wrap.done.assign(std::size(kPrefs), 0);
    }
    if (!g_wrap.done[pref]) {
        g_wrap.label[pref] = Wrap(Font::Bold, kPrefs[pref].label, width);
        g_wrap.help[pref] = Wrap(Font::Normal, HelpText(pref, searching), width);
        g_wrap.done[pref] = 1;
    }
    *label = &g_wrap.label[pref];
    *help = &g_wrap.help[pref];
}

// One line of the sheet: a heading, or a setting with its label, help, control and reset
// (app_tweaks.cpp, SheetRow).
struct PrefRow {
    int pref = -1;  // kPrefs index, or -1 for a heading
    const char* heading = nullptr;
    bool pageHeading = false;
    float y = 0, h = 0;  // in the sheet's content, before scrolling
    Rect control, reset;
};
// Which control has the keyboard, for the focus ring Windows draws around it.
enum class PrefFocus { Sheet, Search, Filter, Number };
struct PrefsPanel {
    int page = 0;
    int focus = -1;       // the current setting (an index into ShownPrefs): its row is lit, the keys change it
    int linkFocus = -1;
    bool reveal = false;  // scroll the current setting into view on the next draw
    bool keyboard = false;  // the current setting was reached with the keys: its switch shows a focus ring
    bool changedOnly = false;  // the scope list's "Changed from default"
    int dropped = -1;     // the open drop-down list: a kPrefs index, kFilterList, or -1
    int dropHover = -1;
    int editing = -1;     // the number setting being typed into, or -1
    std::string editText;
    bool thumbDrag = false;
    float thumbGrab = 0;
    std::wstring search;
    float scroll = 0, contentH = 0, textW = 0;
    Rect panel, caption, captionClose, client, searchBox, filter, title, about, sheet, scrollBar, thumb, resetAll, close, dropList;
    std::vector<Rect> pages, links;
    std::vector<PrefRow> rows;
} g_prefs;
constexpr int kFilterList = -2;
const char* const kFilterNames[] = {"All settings", "Changed from default"};

bool LinkShown(const PrefLink& link) {
    return link.label && g_prefs.search.empty() && (!link.shown || link.shown());
}

bool LinkEnabled(const PrefLink& link) { return !link.enabled || link.enabled(); }

void OpenPreferenceLink(size_t index) {
    const auto& links = kPrefPages[g_prefs.page].links;
    if (index >= std::size(links) || !LinkShown(links[index]) || !LinkEnabled(links[index])) return;
    const PrefLink& link = links[index];
    if (!link.action) ClosePanel();
    link.open();
}

bool Changed(const Pref& t) { return std::fabs(t.get() - t.def) > 1e-4; }

std::string PrefText(const Pref& t, double v) {
    char buf[48];
    switch (t.type) {
        case PrefType::Bool: return kOffOn[v != 0];
        case PrefType::Choice: return t.choices[std::clamp((int)std::lround(v), 0, (int)t.max)];
        case PrefType::Float:
            if (t.unit && !strcmp(t.unit, "dB")) snprintf(buf, sizeof(buf), "%+.1f %s", v, t.unit);
            else if (t.unit) snprintf(buf, sizeof(buf), "%.0f %s", v, t.unit);
            else snprintf(buf, sizeof(buf), "%.2f", v);
            return buf;
    }
    return {};
}

// The settings of themes this build leaves out (theme_palette.h) are never shown.
bool PrefShown(const Pref& t) {
    if (t.key && !discord::Available() && !strcmp(t.key, "discord_status")) return false;  // no application id yet
    return kExtraThemes || !t.key || (strcmp(t.key, "live_fountain") && strcmp(t.key, "robotic_sounds"));
}

// The settings shown: the open page's, or with a search, every one whose words match; with the scope
// set to "Changed from default", only those that differ.
std::vector<int> ShownPrefs() {
    std::vector<int> shown;
    std::wstring query = textcase::Fold(g_prefs.search);
    for (int i = 0; i < (int)std::size(kPrefs); ++i) {
        const Pref& t = kPrefs[i];
        if (!PrefShown(t) || (g_prefs.changedOnly && !Changed(t))) continue;
        if (!query.empty()) {
            std::string text = std::string(t.label) + " " + t.page + " " + (t.key ? t.key : "") + " " + t.help;
            if (textcase::Fold(compat::Widen(text.c_str())).find(query) == std::wstring::npos) continue;
        } else if (g_prefs.page && strcmp(kPrefPages[g_prefs.page].name, t.page)) continue;
        shown.push_back(i);
    }
    // All settings reads as every page in turn, each under its name.
    if (query.empty() && !g_prefs.page)
        std::stable_sort(shown.begin(), shown.end(), [](int a, int b) { return PlaceOf(a).page < PlaceOf(b).page; });
    return shown;
}

// --- Windows' controls, light (the Explorer theme) or dark (DarkMode_Explorer, DarkMode_CFD) -----------

constexpr uint32_t kFocusBlue = 0x0078D7;  // the system highlight: a focused field, a drop-down's pick

struct ControlColors {
    uint32_t fill, hot, pressed, border, text, field, fieldBorder, fieldHot;
};
ControlColors Controls(const Palette& p) {
    if (p.dark) return {0x333333, 0x454545, 0x3A3A3A, 0x9B9B9B, p.text, 0x191919, 0x9B9B9B, 0xC8C8C8};
    return {0xFDFDFD, 0xF5F5F5, 0xE8E8E8, 0xD0D0D0, 0x000000, 0xFFFFFF, 0x7A7A7A, 0x171717};
}

// A push button: rounded, filled, a one-pixel border, the label centred.
void PrefButton(const Rect& r, const std::string& label, const Palette& p, bool enabled = true, bool focused = false) {
    const ControlColors c = Controls(p);
    const bool hot = enabled && r.In(g_mx, g_my);
    FillRound(r, S(4), hot ? c.hot : c.fill);
    FrameRound(r, S(4), focused ? kFocusBlue : c.border);
    DrawText(Font::Normal, label, r.x, TextY(r), enabled ? c.text : p.faint, r.w, 1);
}

// A drop-down list's closed box: the choice and a chevron.
void PrefCombo(const Rect& r, const std::string& text, const Palette& p, bool open) {
    const ControlColors c = Controls(p);
    const bool hot = r.In(g_mx, g_my);
    FillRound(r, S(4), open ? c.pressed : hot ? c.hot : c.fill);
    FrameRound(r, S(4), open ? kFocusBlue : c.border);
    DrawText(Font::Normal, text, r.x + S(8), TextY(r), c.text, r.w - S(32));
    const float ax = r.R() - S(16), ay = std::round(r.y + r.h / 2) - S(1), d = S(4);
    ThickLine(ax - d, ay - d / 2, ax, ay + d / 2, std::max(1.0f, S(1)), p.dark ? 0xD0D0D0 : 0x404040);
    ThickLine(ax, ay + d / 2, ax + d, ay - d / 2, std::max(1.0f, S(1)), p.dark ? 0xD0D0D0 : 0x404040);
}

// An edit field: square, a one-pixel border that darkens on hover and turns blue with the keyboard.
void PrefField(const Rect& r, const std::string& text, const char* cue, const Palette& p, bool focused) {
    const ControlColors c = Controls(p);
    Fill(r, c.field);
    Frame(r, focused ? kFocusBlue : r.In(g_mx, g_my) ? c.fieldHot : c.fieldBorder);
    const float tx = r.x + S(5), ty = TextY(r);
    if (text.empty() && cue) DrawText(Font::Normal, cue, tx, ty, p.sub, r.w - S(10));
    float w = text.empty() ? 0 : DrawText(Font::Normal, text, tx, ty, c.text, r.w - S(10));
    if (focused && SDL_GetTicks() % 1060 < 530) Fill({tx + w, ty, S(1), LineHeight(Font::Normal)}, c.text);
}

// An on/off setting as a switch: the track lit in the accent when on, the knob at that end, and the
// word beside it (app_tweaks.cpp, DrawSwitch).
void PrefSwitch(const Rect& r, bool on, bool focused, const Palette& p) {
    const float trackH = S(20), trackW = S(38), top = r.y + std::round((r.h - trackH) / 2);
    const Rect track{r.x + S(2), top, trackW, trackH};
    FillRound(track, trackH / 2, on ? p.accent : p.border);
    const float knob = trackH - S(6), kx = on ? track.R() - S(3) - knob : track.x + S(3);
    Circle(kx + knob / 2, track.y + S(3) + knob / 2, knob / 2, on ? TextOn(p.accent) : p.text);
    if (focused) {
        for (float i = 0; i < std::max(1.0f, S(2)); ++i)
            FrameRound({track.x - S(2) + i, track.y - S(2) + i, track.w + S(4) - 2 * i, track.h + S(4) - 2 * i}, trackH / 2 + S(2) - i, p.accent);
    }
    DrawText(Font::Normal, on ? "On" : "Off", track.R() + S(10), TextY(r), p.text);
}

// The sheet's scroll bar, as Windows' classic one: arrows at the ends and a thumb sized to the page.
void PrefScrollBar(const Rect& r, float scroll, float content, const Palette& p) {
    const uint32_t track = p.dark ? 0x171717 : 0xF0F0F0, thumb = p.dark ? 0x4D4D4D : 0xCDCDCD,
                   thumbHot = p.dark ? 0x7A7A7A : 0xA6A6A6, arrow = p.dark ? 0x999999 : 0x606060;
    Fill(r, track);
    const float a = r.w;
    for (int end = 0; end < 2; ++end) {
        const float cx = r.x + r.w / 2, cy = end ? r.B() - a / 2 : r.y + a / 2, d = S(3.5f);
        const float s = end ? 1 : -1;
        Polygon({{cx - d, cy - s * d / 2}, {cx + d, cy - s * d / 2}, {cx, cy + s * d / 2}}, arrow);
    }
    const float travel = r.h - 2 * a, h = std::max(S(17), travel * r.h / std::max(content, r.h));
    const float y = r.y + a + (content > r.h ? (travel - h) * scroll / (content - r.h) : 0);
    g_prefs.thumb = {r.x + S(2), y, r.w - S(4), h};
    Fill(g_prefs.thumb, g_prefs.thumbDrag || g_prefs.thumb.In(g_mx, g_my) ? thumbHot : thumb);
}

// --- the window ---------------------------------------------------------------------------------------

// The page list: a caption over each group's first page, the selected page lit with an accent bar and
// in bold, and on the right how many settings there differ from their defaults (DrawPage).
void DrawPageList(const Palette& p, const Rect& list) {
    int changedOn[std::size(kPrefPages)] = {};  // [0] is All settings
    for (int i = 0; i < (int)std::size(kPrefs); ++i)
        if (PrefShown(kPrefs[i]) && Changed(kPrefs[i])) {
            ++changedOn[0];
            if (const int page = PlaceOf(i).page) ++changedOn[page];
        }
    g_prefs.pages.clear();
    float y = list.y;
    for (int i = 0; i < (int)std::size(kPrefPages); ++i) {
        if (kPrefPages[i].group) {
            const Rect caption{list.x, y, list.w, S(30)};
            DrawText(Font::Bold, kPrefPages[i].group, caption.x + S(12), caption.B() - S(5) - LineHeight(Font::Bold), p.sub, caption.w - S(12));
            y += S(30);
        }
        const Rect r{list.x, y, list.w, S(34)};
        const bool on = i == g_prefs.page;
        if (on) {
            Fill(r, p.select);
            Fill({r.x, r.y + S(7), S(3), r.h - S(14)}, p.accent);
        }
        const Rect text{r.x + S(14), r.y, r.w - S(24), r.h};
        if (changedOn[i])
            DrawText(Font::Normal, std::to_string(changedOn[i]), text.x, TextY(text), on ? p.selectText : p.sub, text.w, 2);
        DrawText(on ? Font::Bold : Font::Normal, kPrefPages[i].name ? kPrefPages[i].name : "All settings", text.x, TextY(text),
                 on ? p.selectText : p.text, text.w - (changedOn[i] ? S(24) : 0));
        g_prefs.pages.push_back(r);
        y += S(34);
    }
}

// The rows' heights and places at the sheet's width (LayoutSheet): a setting is its bold label and its
// help on the left, its control and Reset on the right; headings are a line with a rule under it.
void LayoutRows(const std::vector<int>& shown, float width) {
    const float pad = S(12), gap = S(20);
    const float controlW = std::clamp(std::round(width * 2 / 5), S(150), S(280)), cx = width - pad - controlW;
    const float textW = g_prefs.textW = std::max(S(120), cx - gap - pad);
    const bool searching = !g_prefs.search.empty();
    std::vector<PrefRow> rows;
    int lastPage = -1;
    const char* lastHeading = nullptr;
    for (int i : shown) {
        if (!searching) {
            const int page = PlaceOf(i).page;
            if (!g_prefs.page && page != lastPage) {
                rows.push_back({-1, kPrefPages[page].name, true});
                lastHeading = nullptr;
            }
            lastPage = page;
            const char* heading = PlaceOf(i).heading;
            if (heading && heading != lastHeading) rows.push_back({-1, heading});
            lastHeading = heading;
        }
        PrefRow row;
        row.pref = i;
        rows.push_back(row);
    }
    float y = 0;
    const float line = LineHeight(Font::Normal), boldLine = LineHeight(Font::Bold);
    for (PrefRow& row : rows) {
        row.y = y;
        if (row.pref < 0) {
            row.h = row.pageHeading ? S(52) : S(40);
        } else {
            const std::vector<std::string>*label, *help;
            Wrapped(row.pref, textW, searching, &label, &help);
            const float text = S(12) + label->size() * boldLine + S(3) + help->size() * line + S(14);
            const float controls = S(12) + S(26) + S(6) + S(24) + S(12);
            row.h = std::max(text, controls);
            const Pref& t = kPrefs[row.pref];
            row.control = {cx, S(12), t.type == PrefType::Bool ? S(120) : t.type == PrefType::Float ? S(90) : controlW, S(26)};
            row.reset = {cx, S(44), S(84), S(24)};
        }
        y += row.h;
    }
    g_prefs.contentH = y;
    g_prefs.rows = std::move(rows);
}

void DrawSheet(const Palette& p, const std::vector<int>& shown) {
    Rect sheet = g_prefs.sheet;
    const float pad = S(12);
    // The scroll bar takes its width from the rows when they need it, as WS_VSCROLL does.
    LayoutRows(shown, sheet.w);
    g_prefs.scrollBar = {};
    if (g_prefs.contentH > sheet.h) {
        g_prefs.scrollBar = {sheet.R() - S(17), sheet.y, S(17), sheet.h};
        LayoutRows(shown, sheet.w - S(17));
    }
    const float width = sheet.w - g_prefs.scrollBar.w;
    g_prefs.scroll = std::clamp(g_prefs.scroll, 0.0f, std::max(0.0f, g_prefs.contentH - sheet.h));
    const int current = g_prefs.focus >= 0 && g_prefs.focus < (int)shown.size() ? shown[g_prefs.focus] : -1;
    if (g_prefs.reveal) {  // the current row wholly in view, with the heading above it when that fits too
        for (size_t n = 0; n < g_prefs.rows.size(); ++n) {
            const PrefRow& row = g_prefs.rows[n];
            if (row.pref != current || current < 0) continue;
            float top = row.y;
            if (n > 0 && g_prefs.rows[n - 1].pref < 0) top = g_prefs.rows[n - 1].y;
            if (row.y + row.h - top > sheet.h) top = row.y;
            if (top < g_prefs.scroll) g_prefs.scroll = top;
            else if (row.y + row.h > g_prefs.scroll + sheet.h) g_prefs.scroll = row.y + row.h - sheet.h;
        }
        g_prefs.reveal = false;
    }
    SDL_Rect clip{(int)sheet.x, (int)sheet.y, (int)width, (int)sheet.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    if (g_prefs.rows.empty()) {
        const char* note = !g_prefs.search.empty() ? "No setting matches the search."
                           : g_prefs.changedOnly   ? "Nothing has been changed here."
                                                   : "These settings have windows of their own.";
        float y = sheet.y + S(12);
        for (const auto& line : Wrap(Font::Normal, note, width - 2 * pad)) {
            DrawText(Font::Normal, line, sheet.x + pad, y, p.sub);
            y += LineHeight(Font::Normal);
        }
    }
    const bool searching = !g_prefs.search.empty();
    for (size_t n = 0; n < g_prefs.rows.size(); ++n) {
        PrefRow& row = g_prefs.rows[n];
        const float top = sheet.y + row.y - g_prefs.scroll;
        // Places on screen, for the clicks; a row outside the sheet is only measured.
        row.control.x += sheet.x, row.control.y += top;
        row.reset.x += sheet.x, row.reset.y += top;
        if (top >= sheet.B() || top + row.h <= sheet.y) continue;
        if (row.pref < 0) {
            DrawText(row.pageHeading ? Font::Heading : Font::Bold, row.heading, sheet.x + pad, top + row.h - S(30),
                     row.pageHeading ? p.text : p.sub, width - 2 * pad);
            Fill({sheet.x + pad, top + row.h - S(6), width - 2 * pad, std::max(1.0f, S(1))}, p.border);
            continue;
        }
        const Pref& t = kPrefs[row.pref];
        if (row.pref == current) {
            Fill({sheet.x, top, width, row.h}, p.hover);
            Fill({sheet.x, top + S(8), S(3), row.h - S(16)}, p.accent);
        }
        const std::vector<std::string>*label, *help;
        Wrapped(row.pref, g_prefs.textW, searching, &label, &help);
        float ty = top + S(12);
        for (const auto& line : *label) {
            DrawText(Font::Bold, line, sheet.x + pad, ty, p.text);
            ty += LineHeight(Font::Bold);
        }
        ty += S(3);
        for (const auto& line : *help) {
            DrawText(Font::Normal, line, sheet.x + pad, ty, p.sub);
            ty += LineHeight(Font::Normal);
        }
        const double v = t.get();
        if (t.type == PrefType::Bool) {
            PrefSwitch(row.control, v != 0, row.pref == current && g_prefs.keyboard, p);
        } else if (t.type == PrefType::Choice) {
            PrefCombo(row.control, PrefText(t, v), p, g_prefs.dropped == row.pref);
        } else {
            const bool editing = g_prefs.editing == row.pref;
            PrefField(row.control, editing ? g_prefs.editText : NumberText(v), nullptr, p, editing);
            if (t.unit) DrawText(Font::Normal, t.unit, row.control.x + S(98), row.control.y + S(3), p.sub, S(60));
        }
        if (Changed(t)) PrefButton(row.reset, "Reset", p);
        else row.reset = {};
        if (n + 1 < g_prefs.rows.size() && g_prefs.rows[n + 1].pref >= 0)
            Fill({sheet.x + pad, top + row.h - S(1), width - 2 * pad, S(1)}, p.border);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
    if (g_prefs.scrollBar.w > 0) PrefScrollBar(g_prefs.scrollBar, g_prefs.scroll, g_prefs.contentH, p);
}

// An open drop-down's list under its box (or above it, where the window ends first), the current
// choice and the one under the mouse in the system highlight.
void DrawDropList(const Palette& p) {
    if (g_prefs.dropped == -1) return;
    const bool filter = g_prefs.dropped == kFilterList;
    Rect box = g_prefs.filter;
    int count = (int)std::size(kFilterNames), selected = g_prefs.changedOnly;
    if (!filter) {
        const Pref& t = kPrefs[g_prefs.dropped];
        count = (int)t.max + 1;
        selected = (int)std::lround(t.get());
        box = {};
        for (const PrefRow& row : g_prefs.rows)
            if (row.pref == g_prefs.dropped) box = row.control;
        if (box.w <= 0 || box.B() <= g_prefs.sheet.y || box.y >= g_prefs.sheet.B()) {  // scrolled away
            g_prefs.dropped = -1;
            return;
        }
    }
    const ControlColors c = Controls(p);
    const float itemH = LineHeight(Font::Normal) + S(4);
    Rect list{box.x, box.B() + S(1), box.w, itemH * count + S(2)};
    if (list.B() > g_prefs.client.B() && box.y - list.h - S(1) >= g_prefs.client.y) list.y = box.y - list.h - S(1);
    g_prefs.dropList = list;
    Fill(list, c.field);
    Frame(list, p.dark ? 0x9B9B9B : 0x646464);
    for (int i = 0; i < count; ++i) {
        const Rect r{list.x + S(1), list.y + S(1) + i * itemH, list.w - S(2), itemH};
        const bool lit = g_prefs.dropHover >= 0 ? i == g_prefs.dropHover : i == selected;
        if (lit) Fill(r, kFocusBlue);
        const char* name = filter ? kFilterNames[i] : kPrefs[g_prefs.dropped].choices[i];
        DrawText(Font::Normal, name, r.x + S(4), TextY(r), lit ? 0xFFFFFF : c.text, r.w - S(8));
    }
}

void DrawPrefs(const Palette& p) {
    // A window over the player, as Windows' own Preferences window: a caption, then the 900 by 660
    // client area laid out as LayoutPreferences lays it out.
    const Rect out = OutputRect();
    Fill(out, p.bg, 190);
    const float captionH = S(31);
    const float cw = std::min(out.w - S(16) - 2, S(900)), ch = std::min(out.h - S(16) - captionH - 2, S(660));
    Rect& panel = g_prefs.panel = {std::round(out.x + (out.w - cw - 2) / 2), std::round(out.y + (out.h - ch - captionH - 2) / 2), cw + 2, ch + captionH + 2};
    Fill(panel, p.surface);
    Frame(panel, p.dark ? 0x505050 : 0xA0A0A0);
    const Rect caption = g_prefs.caption = {panel.x + 1, panel.y + 1, panel.w - 2, captionH};
    Fill(caption, p.dark ? p.surface : 0xFFFFFF);
    DrawText(Font::Menu, "Preferences", caption.x + S(12), TextY(caption, Font::Menu), p.dark ? p.text : 0x000000);
    const Rect x = g_prefs.captionClose = {caption.R() - S(46), caption.y, S(46), caption.h};
    const bool closeHot = x.In(g_mx, g_my);
    if (closeHot) Fill(x, 0xC42B1C);
    DrawFluentIcon("dismiss", x.x + x.w / 2, x.y + x.h / 2, S(10), closeHot ? 0xFFFFFF : p.dark ? p.text : 0x000000);
    const Rect c = g_prefs.client = {panel.x + 1, caption.B(), panel.w - 2, ch};

    const float pad = S(16), navW = S(188), buttonH = S(28), bottom = c.B() - pad - buttonH;
    DrawPageList(p, {c.x + pad, c.y + pad, navW, std::max(c.h - pad * 2, S(80))});
    Fill({c.x + pad + navW + S(10), c.y + pad, std::max(1.0f, S(1)), c.h - 2 * pad}, p.border);
    const float x0 = c.x + pad + navW + S(22), width = c.R() - pad - x0, filterW = S(170);
    float y = c.y + pad;
    g_prefs.searchBox = {x0, y, width - filterW - S(8), S(26)};
    PrefField(g_prefs.searchBox, Utf8(g_prefs.search), "Search every page", p, g_prefs.editing < 0 && g_prefs.dropped == -1);
    g_prefs.filter = {x0 + width - filterW, y, filterW, S(26)};
    PrefCombo(g_prefs.filter, kFilterNames[g_prefs.changedOnly], p, g_prefs.dropped == kFilterList);
    y += S(42);
    const auto shown = ShownPrefs();
    const bool searching = !g_prefs.search.empty();
    g_prefs.title = {x0, y, width, S(30)};
    DrawText(Font::Heading, searching ? "Search" : kPrefPages[g_prefs.page].name ? kPrefPages[g_prefs.page].name : "All settings",
             x0, y, p.text, width);
    y += S(32);
    g_prefs.about = {x0, y, width, S(20)};
    const std::string about = !searching ? kPrefPages[g_prefs.page].about
                              : shown.size() == 1 ? "1 setting matches, from every page."
                                                  : std::to_string(shown.size()) + " settings match, from every page.";
    DrawText(Font::Normal, about, x0, y, p.sub, width);
    y += S(30);
    // The page's links to the windows that hold the rest of its area's settings.
    const auto& links = kPrefPages[g_prefs.page].links;
    int linkCount = 0;
    for (const PrefLink& link : links) linkCount += LinkShown(link);
    if (g_prefs.linkFocus >= 0 && (!LinkShown(links[g_prefs.linkFocus]) || !LinkEnabled(links[g_prefs.linkFocus])))
        g_prefs.linkFocus = -1;
    const float linkW = std::min(S(190), (width - S(8) * std::max(0, linkCount - 1)) / std::max(1, linkCount));
    g_prefs.links.assign(std::size(links), {});
    float lx = x0;
    for (size_t i = 0; i < std::size(links); ++i) {
        if (!LinkShown(links[i])) continue;
        g_prefs.links[i] = {lx, y, linkW, buttonH};
        PrefButton(g_prefs.links[i], links[i].label, p, LinkEnabled(links[i]), (int)i == g_prefs.linkFocus);
        lx += linkW + S(8);
    }
    if (linkCount) y += buttonH + S(10);
    // The sheet reaches a row's padding left of the title, so labels line up under it.
    g_prefs.sheet = {x0 - S(12), y, width + S(12), std::max(bottom - S(12) - y, S(80))};
    DrawSheet(p, shown);
    g_prefs.resetAll = {x0, bottom, S(190), buttonH};
    g_prefs.close = {c.R() - pad - S(90), bottom, S(90), buttonH};
    PrefButton(g_prefs.resetAll, "Reset all to defaults", p);
    PrefButton(g_prefs.close, "Close", p);
    DrawDropList(p);
}

void SetPref(const Pref& t, double v) {
    v = std::clamp(v, (double)t.min, (double)t.max);
    if (t.type == PrefType::Float) v = std::round(v / t.step) * t.step;
    if (std::fabs(v - t.get()) > 1e-6) t.set(v);
    g.redraw = true;
}

void ClampPrefsScroll() {
    g_prefs.scroll = std::clamp(g_prefs.scroll, 0.0f, std::max(0.0f, g_prefs.contentH - g_prefs.sheet.h));
}

// A typed number goes in when Enter is pressed or the field is left, clamped to its range
// (CommitNumber); text that is not a number leaves the value as it was.
void CommitNumber() {
    if (g_prefs.editing < 0) return;
    const Pref& t = kPrefs[g_prefs.editing];
    char* end = nullptr;
    const double v = strtod(g_prefs.editText.c_str(), &end);
    if (end != g_prefs.editText.c_str()) SetPref(t, v);
    g_prefs.editing = -1;
    g_prefs.editText.clear();
    g.redraw = true;
}

void FocusPref(int pref, bool keyboard) {
    auto shown = ShownPrefs();
    auto it = std::find(shown.begin(), shown.end(), pref);
    g_prefs.focus = it == shown.end() ? -1 : (int)(it - shown.begin());
    g_prefs.keyboard = keyboard;
    g_prefs.linkFocus = -1;
}

void ChoosePage(int page) {
    g_prefs.page = page;
    g_prefs.search.clear();  // choosing a page ends the search
    g_prefs.scroll = 0;
    g_prefs.focus = -1;
    g_prefs.linkFocus = -1;
}

void PrefsMouseDown(float x, float y) {
    g.redraw = true;  // a list opens, a page changes or a value flips: show it on this frame
    // An open drop-down takes the click: a choice picks, anywhere else closes it.
    if (g_prefs.dropped != -1) {
        const int dropped = g_prefs.dropped;
        g_prefs.dropped = -1;
        const Rect& list = g_prefs.dropList;
        if (list.In(x, y)) {
            const int i = (int)((y - list.y - S(1)) / (LineHeight(Font::Normal) + S(4)));
            if (dropped == kFilterList) {
                g_prefs.changedOnly = i == 1;
                g_prefs.scroll = 0;
                g_prefs.focus = -1;
            } else {
                SetPref(kPrefs[dropped], std::clamp(i, 0, (int)kPrefs[dropped].max));
            }
        }
        g.redraw = true;
        return;
    }
    CommitNumber();
    if (!g_prefs.panel.In(x, y) || g_prefs.close.In(x, y) || g_prefs.captionClose.In(x, y)) {
        ClosePanel();
        return;
    }
    for (size_t i = 0; i < g_prefs.pages.size(); ++i)
        if (g_prefs.pages[i].In(x, y)) return ChoosePage((int)i);
    if (g_prefs.filter.In(x, y)) {
        g_prefs.dropped = kFilterList;
        g_prefs.dropHover = -1;
        return;
    }
    if (g_prefs.resetAll.In(x, y)) {
        int count = 0;
        for (int i : ShownPrefs())
            if (Changed(kPrefs[i])) {
                SetPref(kPrefs[i], kPrefs[i].def);
                ++count;
            }
        ShowMessage(count == 1 ? L"Reset 1 setting to its default." : L"Reset " + std::to_wstring(count) + L" settings to their defaults.");
        return;
    }
    if (g_prefs.search.empty())
        for (size_t i = 0; i < g_prefs.links.size(); ++i)
            if (g_prefs.links[i].w > 0 && g_prefs.links[i].In(x, y)) {
                g_prefs.linkFocus = (int)i;
                OpenPreferenceLink(i);
                return;
            }
    if (g_prefs.scrollBar.w > 0 && g_prefs.scrollBar.In(x, y)) {
        const Rect& sb = g_prefs.scrollBar;
        if (g_prefs.thumb.In(x, y)) {
            g_prefs.thumbDrag = true;
            g_prefs.thumbGrab = y - g_prefs.thumb.y;
        } else if (y < sb.y + sb.w) g_prefs.scroll -= S(24);
        else if (y >= sb.B() - sb.w) g_prefs.scroll += S(24);
        else g_prefs.scroll += y < g_prefs.thumb.y ? -g_prefs.sheet.h : g_prefs.sheet.h;
        ClampPrefsScroll();
        return;
    }
    if (!g_prefs.sheet.In(x, y)) return;
    for (const PrefRow& row : g_prefs.rows) {
        if (row.pref < 0) continue;
        const Pref& t = kPrefs[row.pref];
        const float top = g_prefs.sheet.y + row.y - g_prefs.scroll;
        if (y < top || y >= top + row.h) continue;
        FocusPref(row.pref, false);  // a click on a row's words makes it current
        if (row.control.In(x, y)) {
            if (t.type == PrefType::Bool) SetPref(t, t.get() != 0 ? 0 : 1);
            else if (t.type == PrefType::Choice) {
                g_prefs.dropped = row.pref;
                g_prefs.dropHover = -1;
            } else {
                g_prefs.editing = row.pref;
                g_prefs.editText = NumberText(t.get());
            }
        } else if (row.reset.w > 0 && row.reset.In(x, y)) {
            SetPref(t, t.def);
        }
        return;
    }
}

void PrefsMouseMove(float x, float y) {
    if (g_prefs.thumbDrag) {
        const Rect& sb = g_prefs.scrollBar;
        const float travel = sb.h - 2 * sb.w - g_prefs.thumb.h;
        if (travel > 0) g_prefs.scroll = (y - g_prefs.thumbGrab - sb.y - sb.w) / travel * (g_prefs.contentH - g_prefs.sheet.h);
        ClampPrefsScroll();
        g.redraw = true;
    }
    if (g_prefs.dropped != -1) {
        const Rect& list = g_prefs.dropList;
        const int hover = list.In(x, y) ? (int)((y - list.y - S(1)) / (LineHeight(Font::Normal) + S(4))) : -1;
        if (hover != g_prefs.dropHover) g_prefs.dropHover = hover, g.redraw = true;
    }
}

void PrefsMouseUp() { g_prefs.thumbDrag = false; }

// Typing searches (or goes into the number being typed); Up and Down pick a setting, Left and Right
// step it (Space flips an on/off one), Delete resets it, Escape closes an open list, then clears the
// search, then closes. Tab visits enabled actions; Enter or Space runs the focused action, and
// Up/Down returns to the settings.
void PrefsKey(const SDL_KeyboardEvent& e) {
    auto shown = ShownPrefs();
    int& focus = g_prefs.focus;
    const Pref* t = focus >= 0 && focus < (int)shown.size() ? &kPrefs[shown[focus]] : nullptr;
    if (g_prefs.dropped != -1) {  // an open list: Up and Down move through it, Enter picks, Escape closes
        const bool filter = g_prefs.dropped == kFilterList;
        const int count = filter ? (int)std::size(kFilterNames) : (int)kPrefs[g_prefs.dropped].max + 1;
        int& at = g_prefs.dropHover;
        if (at < 0) at = filter ? (int)g_prefs.changedOnly : (int)std::lround(kPrefs[g_prefs.dropped].get());
        if (e.key == SDLK_UP || e.key == SDLK_DOWN) at = std::clamp(at + (e.key == SDLK_UP ? -1 : 1), 0, count - 1);
        else if (e.key == SDLK_RETURN || e.key == SDLK_SPACE) {
            if (filter) g_prefs.changedOnly = at == 1, g_prefs.focus = -1;
            else SetPref(kPrefs[g_prefs.dropped], at);
            g_prefs.dropped = -1;
        } else if (e.key == SDLK_ESCAPE) g_prefs.dropped = -1;
        g.redraw = true;
        return;
    }
    if (g_prefs.editing >= 0) {  // a number being typed
        if (e.key == SDLK_RETURN || e.key == SDLK_TAB) CommitNumber();
        else if (e.key == SDLK_ESCAPE) g_prefs.editing = -1, g_prefs.editText.clear();
        else if (e.key == SDLK_BACKSPACE && !g_prefs.editText.empty()) g_prefs.editText.pop_back();
        g.redraw = true;
        return;
    }
    switch (e.key) {
        case SDLK_TAB: {
            const auto& links = kPrefPages[g_prefs.page].links;
            const int step = e.mod & SDL_KMOD_SHIFT ? -1 : 1;
            int at = g_prefs.linkFocus;
            if (at < 0) at = step > 0 ? -1 : (int)std::size(links);
            for (size_t n = 0; n < std::size(links); ++n) {
                at = (at + step + (int)std::size(links)) % (int)std::size(links);
                if (LinkShown(links[at]) && LinkEnabled(links[at])) { g_prefs.linkFocus = at; break; }
            }
            break;
        }
        case SDLK_RETURN:
            if (g_prefs.linkFocus >= 0) OpenPreferenceLink((size_t)g_prefs.linkFocus);
            else if (t && t->type == PrefType::Choice) g_prefs.dropped = shown[focus], g_prefs.dropHover = -1;
            else if (t && t->type == PrefType::Float) g_prefs.editing = shown[focus], g_prefs.editText = NumberText(t->get());
            break;
        case SDLK_ESCAPE:
            if (!g_prefs.search.empty()) {
                g_prefs.search.clear();
                focus = -1;
            } else ClosePanel();
            break;
        case SDLK_BACKSPACE:
            if (e.mod & SDL_KMOD_CTRL) g_prefs.search.clear();
            else if (!g_prefs.search.empty()) g_prefs.search.pop_back();
            g_prefs.scroll = 0;
            focus = -1;
            break;
        case SDLK_UP:
        case SDLK_DOWN:
            g_prefs.linkFocus = -1;
            if (!shown.empty()) focus = std::clamp(focus + (e.key == SDLK_UP ? -1 : 1), 0, (int)shown.size() - 1);
            g_prefs.keyboard = true;
            g_prefs.reveal = true;
            break;
        case SDLK_LEFT:
        case SDLK_RIGHT:
            if (t && g_prefs.linkFocus < 0) SetPref(*t, t->get() + (e.key == SDLK_RIGHT ? t->step : -t->step));
            break;
        case SDLK_SPACE:
            if (g_prefs.linkFocus >= 0) OpenPreferenceLink((size_t)g_prefs.linkFocus);
            else if (t && t->type == PrefType::Bool) SetPref(*t, t->get() != 0 ? 0 : 1);
            break;
        case SDLK_DELETE:
            if (t && g_prefs.linkFocus < 0) SetPref(*t, t->def);
            break;
        case SDLK_PAGEUP: g_prefs.scroll -= g_prefs.sheet.h; break;
        case SDLK_PAGEDOWN: g_prefs.scroll += g_prefs.sheet.h; break;
        default: break;
    }
    ClampPrefsScroll();
    g.redraw = true;
}

void PrefsText(const char* text) {
    if (g_prefs.dropped != -1) return;
    if (g_prefs.editing >= 0) {  // a number: digits, a sign and a point
        for (const char* c = text; *c; ++c)
            if (isdigit((unsigned char)*c) || *c == '.' || *c == '-' || *c == '+') g_prefs.editText += *c;
        g.redraw = true;
        return;
    }
    if (g_prefs.focus >= 0 && !strcmp(text, " ")) return;  // Space flips the current setting instead
    g_prefs.search += compat::Widen(text);
    g_prefs.page = 0;  // a search covers every page, so the page list says so
    g_prefs.scroll = 0;
    g_prefs.focus = -1;
    g.redraw = true;
}

void PrefsWheel(float, float, float dy) {
    if (g_prefs.dropped != -1) return;
    g_prefs.scroll -= dy * S(60);
    ClampPrefsScroll();
}

}  // namespace

size_t PrefCount() { return std::size(kPrefs); }
bool PrefListed(int i) { return PrefShown(kPrefs[i]); }

PaletteCommand PrefCommand(int i) {
    const Pref& t = kPrefs[i];
    return {t.label, std::string("Preferences \xE2\x80\xBA ") + t.page, PrefText(t, t.get()), true, false};
}

std::wstring PrefSearchText(int i) {
    const Pref& t = kPrefs[i];
    return textcase::Fold(compat::Widen((std::string(t.label) + " " + t.page + " preferences " + (t.key ? t.key : "")).c_str()));
}

void RunPref(int i) {
    const Pref& t = kPrefs[i];
    if (t.type == PrefType::Bool) {
        SetPref(t, t.get() != 0 ? 0 : 1);
        ShowMessage(compat::Widen(t.label) + (t.get() != 0 ? L": on." : L": off."));
    } else {
        OpenPreferences();
        g_prefs.search = compat::Widen(t.label);
    }
}

const Ops kPrefsOps{
    .draw = DrawPrefs,
    .mouseDown = [](float x, float y, int) { PrefsMouseDown(x, y); },
    .mouseMove = PrefsMouseMove,
    .mouseUp = PrefsMouseUp,
    .wheel = PrefsWheel,
    .key = PrefsKey,
    .text = PrefsText,
    .typing = true,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OpenPreferences() {
    g_kind = Kind::Prefs;
    g_prefs.search.clear();
    g_prefs.scroll = 0;
    g_prefs.focus = -1;
    g_prefs.linkFocus = -1;
    g_prefs.dropped = g_prefs.editing = -1;
    g_prefs.changedOnly = false;
    SDL_StartTextInput(g.window);  // typing searches
    g.redraw = true;
}

int CheckPreferences() {
    const auto saved = g_prefs;
    const auto kind = g_kind;
    const bool running = g.rgRunning, automatic = g.rgAuto, cancelled = g.rgCancel;
    OpenPreferences();
    for (size_t i = 1; i < std::size(kPrefPages); ++i)
        if (std::string_view(kPrefPages[i].name) == "ReplayGain") g_prefs.page = (int)i;
    g.rgRunning = false;
    g.rgAuto = false;
    DrawPrefs(CurrentPalette());
    const auto& links = kPrefPages[g_prefs.page].links;
    bool idle = g_prefs.links.size() == 4 && g_prefs.links[0].w > 0 && g_prefs.links[1].w > 0 &&
                g_prefs.links[2].w > 0 && g_prefs.links[3].w == 0 &&
                LinkEnabled(links[2]) == replaygain::HasLibraryTracks();
    g.rgRunning = true;
    DrawPrefs(CurrentPalette());
    bool active = !LinkEnabled(links[0]) && !LinkEnabled(links[1]) && !LinkEnabled(links[2]) &&
                  LinkShown(links[3]) && LinkEnabled(links[3]);
    float right = g_prefs.about.x;
    for (const auto& button : g_prefs.links) {
        active &= button.w > 0 && button.x >= right && button.R() <= g_prefs.client.R() &&
                  button.y >= g_prefs.about.B() && button.B() <= g_prefs.sheet.y;
        right = button.R();
    }
    SDL_KeyboardEvent key{};
    key.key = SDLK_TAB;
    PrefsKey(key);
    const bool tab = g_prefs.linkFocus == 3;
    key.key = SDLK_SPACE;
    PrefsKey(key);
    const bool keyboard = tab && g.rgCancel && g_kind == Kind::Prefs;
    g.rgRunning = running;
    g.rgAuto = automatic;
    g.rgCancel = cancelled;
    g_prefs = saved;
    g_kind = kind;
    if (kind != Kind::Prefs) SDL_StopTextInput(g.window);
    printf("%s ReplayGain Preferences exposes three scan scopes and live cancellation without overlap\n",
           idle && active ? "ok  " : "FAIL");
    printf("%s ReplayGain Preferences supports Tab and Space without closing the panel\n", keyboard ? "ok  " : "FAIL");
    return (!idle || !active) + !keyboard;
}

}  // namespace lx
