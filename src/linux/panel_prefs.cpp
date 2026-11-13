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
     "Sends the artist's name to musicbrainz.org, then asks wikidata.org and en.wikipedia.org for the article it names.",
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
    {"convert_recycle", "Library", "Send converted originals to the Recycle Bin",
     "After converting to .cord, the original files go to the Recycle Bin instead of staying where they are.", PrefType::Bool,
     0, 1, 1, 1, [] { return (double)Cfg().convertRecycle; }, [](double v) { Cfg().convertRecycle = v != 0; Saved(); }},
    {"convert_show_originals", "Library", "Show kept originals in the library",
     "Originals kept after a conversion appear in the library beside their .cord copies.", PrefType::Bool,
     0, 1, 0, 1, [] { return (double)Cfg().convertShowOriginals; }, [](double v) { SetConvertShowOriginals(v != 0); Saved(); }},
    {"convert_undo_days", "Library", "Conversions can be undone for",
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
    const char* name;  // null for All settings
    PrefLink links[4];
    const char* group = nullptr;  // the caption above the page in the list, where a group starts
};
// In the Windows build's groups (app_tweaks.cpp): sound, look, the collection, and the rest.
const PrefPage kPrefPages[] = {
    {nullptr, {}},
    {"Playback", {}, "Sound"},
    {"Output", {{"Output device...", OpenOutputDevice}}},
    {"Headphones", {{"Headphones...", OpenHeadphones}}},
    {"ReplayGain",
     {{"Scan selection", [] { ScanReplayGain(SelectedTracks()); },
       [] { return (!g.rgRunning || g.rgAuto) &&
                   std::any_of(g.selected.begin(), g.selected.end(), [](char value) { return value != 0; }); }, nullptr, true},
      {"Scan playlist", [] { ScanReplayGain(PlaylistTracks(Active())); },
       [] { return (!g.rgRunning || g.rgAuto) && !g.rows.empty(); }, nullptr, true},
      {"Scan library", [] { ScanReplayGain(replaygain::LibraryTracks()); },
       [] { return (!g.rgRunning || g.rgAuto) && replaygain::HasLibraryTracks(); }, nullptr, true},
      {"Cancel scan", CancelReplayGain, nullptr, [] { return g.rgRunning && !g.rgAuto; }, true}}},
    {"Effects", {{"Equalizer...", OpenEqualizer}}},
    {"View", {{"Title formatting...", OpenFormat}, {"Customize layout...", OpenLayout}}, "Look"},
    {"Library", {{"Library folders...", OpenLibraryFolders}, {"Web server...", OpenWebServer}}, "Collection"},
    {"Online", {{"Fetch missing covers...", FetchMissingCovers, [] { return !CoverFetchBusy(); }}}},
    {"Scrobbling", {{"Scrobbling...", OpenListenBrainz}}},
    {"Startup", {{"Startup defaults...", OpenStartupDefaults}}, "General"},
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
    {"seek_bar", "Seek bar"}, {"wave_color", "Colours"}, {"smooth_seek", "Shape"}, {"group_albums", "Window"},
    {"sound_effects", "Sounds"}, {"musicbrainz_lookup", "Adding music"}, {"cover_gap", "Sizes"},
    {"web_start", "Outside cordinal"}, {"convert_recycle", "Conversion"},
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

// Each setting's help, wrapped to the list's width. Wrapping measures every word, which for All settings
// is thousands of measurements, so the lines are kept until the width or the font changes.
struct HelpCache {
    float width = -1, sample = -1;
    std::vector<std::vector<std::string>> lines;
    std::vector<char> done;
} g_help;
const std::vector<std::string>& HelpLines(int pref, float width) {
    const float sample = TextWidth(Font::Small, "The quick brown fox jumps over the lazy dog");
    if (g_help.width != width || g_help.sample != sample) {
        g_help.width = width;
        g_help.sample = sample;
        g_help.lines.assign(std::size(kPrefs), {});
        g_help.done.assign(std::size(kPrefs), 0);
    }
    if (!g_help.done[pref]) {
        g_help.lines[pref] = Wrap(Font::Small, kPrefs[pref].help, width);
        g_help.done[pref] = 1;
    }
    return g_help.lines[pref];
}

struct PrefRow {
    int pref;
    Rect less, more, toggle, reset;
};
struct PrefsPanel {
    int page = 0;
    int focus = -1;       // the shown setting the keys change (an index into ShownPrefs), or -1
    int linkFocus = -1;
    bool reveal = false;  // scroll the focused setting into view on the next draw
    std::wstring search;
    float scroll = 0, contentH = 0;
    Rect panel, searchBox, list, resetAll, close;
    std::vector<Rect> pages, links;
    std::vector<PrefRow> rows;
} g_prefs;

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

// The settings shown: the open page's, or with a search, every one whose words match.
std::vector<int> ShownPrefs() {
    std::vector<int> shown;
    std::wstring query = textcase::Fold(g_prefs.search);
    for (int i = 0; i < (int)std::size(kPrefs); ++i) {
        const Pref& t = kPrefs[i];
        if (!PrefShown(t)) continue;
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
void DrawPrefs(const Palette& p) {
    Rect panel = g_prefs.panel = DrawFrame(p, S(860), S(660), "Preferences");
    float pad = S(16), x0 = panel.x + pad, right = panel.R() - pad, top = panel.y + S(52);
    // The search, top right, as on Windows.
    Rect& box = g_prefs.searchBox = {right - S(260), panel.y + S(10), S(260), S(28)};
    Fill(box, p.surface2);
    Frame(box, p.accent);
    Fill({box.x + S(1), box.B() - S(1), box.w - S(2), S(1)}, p.accent);
    if (g_prefs.search.empty()) DrawText(Font::Normal, "Search settings", box.x + S(6), TextY(box), p.sub, box.w - S(12));
    float w = DrawText(Font::Normal, Utf8(g_prefs.search), box.x + S(6), TextY(box), p.text, box.w - S(12));
    if (SDL_GetTicks() % 1060 < 530) Fill({box.x + S(6) + w, TextY(box), S(1), LineHeight(Font::Normal)}, p.text);
    // The pages, each with how many of its settings differ from their defaults.
    int linkCount = 0;
    for (const PrefLink& link : kPrefPages[g_prefs.page].links) linkCount += LinkShown(link);
    float bottom = panel.B() - pad - S(28) - S(12) - (linkCount ? S(40) : 0);
    g_prefs.pages.clear();
    int changedOn[std::size(kPrefPages)] = {};  // [0] is All settings
    for (int i = 0; i < (int)std::size(kPrefs); ++i)
        if (PrefShown(kPrefs[i]) && Changed(kPrefs[i])) {
            ++changedOn[0];
            if (const int page = PlaceOf(i).page) ++changedOn[page];
        }
    float py = top;
    for (int i = 0; i < (int)std::size(kPrefPages); ++i) {
        if (kPrefPages[i].group) {  // the group's caption, above its first page
            DrawText(Font::Bold, kPrefPages[i].group, x0 + S(12), py + S(8), p.sub, S(148));
            py += S(30);
        }
        Rect r{x0, py, S(160), S(30)};
        bool on = g_prefs.search.empty() && i == g_prefs.page;
        if (on) Fill(r, p.select);
        else if (r.In(g_mx, g_my)) Fill(r, p.hover);
        if (on) Fill({r.x, r.y + S(6), S(3), r.h - S(12)}, p.accent);
        const int changed = changedOn[i];
        DrawText(Font::Normal, kPrefPages[i].name ? kPrefPages[i].name : "All settings", r.x + S(12), TextY(r),
                 on ? p.selectText : p.text, r.w - S(40));
        if (changed) DrawText(Font::Small, std::to_string(changed), r.x, TextY(r, Font::Small), on ? p.selectText : p.sub, r.w - S(10), 2);
        g_prefs.pages.push_back(r);
        py += S(32);
    }
    Fill({x0 + S(172), top, S(1), bottom - top}, p.border);
    // The settings, scrolled; each is its label, its help, its default and key, and its control.
    Rect list = g_prefs.list = {x0 + S(188), top, right - x0 - S(188), bottom - top};
    auto shown = ShownPrefs();
    float controlW = S(250), textW = list.w - controlW - S(20);
    SDL_Rect clip{(int)list.x, (int)list.y, (int)list.w, (int)list.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    float y = list.y - g_prefs.scroll;
    g_prefs.rows.clear();
    if (shown.empty())
        DrawText(Font::Normal, "No setting matches the search.", list.x, list.y + S(8), p.sub);
    int lastPage = -1;
    const char* lastHeading = nullptr;
    for (size_t n = 0; n < shown.size(); ++n) {
        int i = shown[n];
        const Pref& t = kPrefs[i];
        // Headings, as on Windows: All settings names each page, and pages have their own sections.
        if (g_prefs.search.empty()) {
            const int page = PlaceOf(i).page;
            if (!g_prefs.page && page != lastPage) {
                DrawText(Font::Title, kPrefPages[page].name, list.x, y + S(16), p.text, list.w);
                y += S(52);
                Fill({list.x, y - S(6), list.w, S(1)}, p.border);
                lastHeading = nullptr;
            }
            lastPage = page;
            const char* heading = PlaceOf(i).heading;
            if (heading && heading != lastHeading) {
                DrawText(Font::Bold, heading, list.x, y + S(12), p.sub, list.w);
                y += S(40);
                Fill({list.x, y - S(6), list.w, S(1)}, p.border);
            }
            lastHeading = heading;
        }
        const auto& help = HelpLines(i, textW);
        float h = S(12) + LineHeight(Font::Normal) + S(4) + (help.size() + 1) * LineHeight(Font::Small) + S(12);
        PrefRow row{i};
        if ((int)n == g_prefs.focus) {
            Fill({list.x - S(10), y + S(8), S(3), h - S(16)}, p.accent);
            if (g_prefs.reveal) {  // corrected on the next frame
                if (y < list.y) g_prefs.scroll -= list.y - y;
                else if (y + h > list.B()) g_prefs.scroll += y + h - list.B();
                g_prefs.reveal = false;
                g.redraw = true;
            }
        }
        // A row outside the list is only measured: drawn and clipped away, it cost as much as one shown.
        if (y + h <= list.y || y >= list.B()) {
            g_prefs.rows.push_back(row);
            y += h;
            continue;
        }
        std::string meta = "Default: " + PrefText(t, t.def) + (t.key ? std::string("   Key: ") + t.key : std::string());
        float ty = y + S(12);
        DrawText(Font::Normal, t.label, list.x, ty, p.text, textW);
        ty += LineHeight(Font::Normal) + S(4);
        for (auto& line : help) {
            DrawText(Font::Small, line, list.x, ty, p.sub, textW);
            ty += LineHeight(Font::Small);
        }
        DrawText(Font::Small, meta, list.x, ty, p.faint, textW);
        float cx = list.R() - controlW, cy = y + S(10);
        double v = t.get();
        if (t.type == PrefType::Bool) {
            row.toggle = {cx, cy, controlW, S(26)};
            DrawCheck(row.toggle, v != 0 ? "On" : "Off", v != 0, p);
        } else {
            // A stepper: the value between buttons that step it down and up (or through the choices).
            row.less = {cx, cy, S(28), S(26)};
            row.more = {list.R() - S(28), cy, S(28), S(26)};
            DrawButton(row.less, t.type == PrefType::Choice ? "<" : "-", p, false, v > t.min + 1e-4);
            DrawButton(row.more, t.type == PrefType::Choice ? ">" : "+", p, false, v < t.max - 1e-4);
            Rect value{row.less.R(), cy, row.more.x - row.less.R(), S(26)};
            DrawText(Font::Normal, PrefText(t, v), value.x + S(4), TextY(value), p.text, value.w - S(8), 1);
        }
        if (Changed(t)) {
            row.reset = {list.R() - S(60), cy + S(32), S(60), S(20)};
            bool hot = row.reset.In(g_mx, g_my);
            DrawText(Font::Small, "Reset", row.reset.x, TextY(row.reset, Font::Small), hot ? p.accent2 : p.accent, row.reset.w, 2);
        }
        g_prefs.rows.push_back(row);
        y += h;
        Fill({list.x, y - S(1), list.w, S(1)}, p.border);
    }
    g_prefs.contentH = y + g_prefs.scroll - list.y;
    SDL_SetRenderClipRect(g.renderer, nullptr);
    // Links and actions have their own row above the footer, so scans never overlap the resets.
    float by = panel.B() - pad - S(28);
    const auto& links = kPrefPages[g_prefs.page].links;
    g_prefs.links.assign(std::size(links), {});
    if (g_prefs.linkFocus >= 0 && (!LinkShown(links[g_prefs.linkFocus]) || !LinkEnabled(links[g_prefs.linkFocus])))
        g_prefs.linkFocus = -1;
    float lx = list.x;
    float maxLinkW = (list.w - S(8) * std::max(0, linkCount - 1)) / std::max(1, linkCount);
    for (size_t i = 0; i < std::size(links); ++i) {
        const PrefLink& link = links[i];
        if (!LinkShown(link)) continue;
        Rect r{lx, by - S(40), std::min(TextWidth(Font::Normal, link.label) + S(28), maxLinkW), S(28)};
        DrawButton(r, link.label, p, false, LinkEnabled(link));
        if ((int)i == g_prefs.linkFocus) Frame(r, p.accent);
        g_prefs.links[i] = r;
        lx = r.R() + S(8);
    }
    bool any = false;
    for (int i : shown) any = any || Changed(kPrefs[i]);
    const char* resetLabel = !g_prefs.search.empty() ? "Reset these to defaults"
                             : g_prefs.page         ? "Reset page to defaults"
                                                    : "Reset all to defaults";
    g_prefs.close = {right - S(90), by, S(90), S(28)};
    g_prefs.resetAll = {g_prefs.close.x - S(8) - S(190), by, S(190), S(28)};
    DrawButton(g_prefs.resetAll, resetLabel, p, false, any);
    DrawButton(g_prefs.close, "Close", p, true);
}

void SetPref(const Pref& t, double v) {
    v = std::clamp(v, (double)t.min, (double)t.max);
    if (t.type == PrefType::Float) v = std::round(v / t.step) * t.step;
    if (std::fabs(v - t.get()) > 1e-6) t.set(v);
    g.redraw = true;
}

void ClampPrefsScroll() {
    g_prefs.scroll = std::clamp(g_prefs.scroll, 0.0f, std::max(0.0f, g_prefs.contentH - g_prefs.list.h));
}

void PrefsMouseDown(float x, float y) {
    if (!g_prefs.panel.In(x, y) || g_prefs.close.In(x, y)) {
        ClosePanel();
        return;
    }
    for (size_t i = 0; i < g_prefs.pages.size(); ++i)
        if (g_prefs.pages[i].In(x, y)) {
            g_prefs.page = (int)i;
            g_prefs.search.clear();
            g_prefs.scroll = 0;
            g_prefs.focus = -1;
            g_prefs.linkFocus = -1;
            return;
        }
    if (g_prefs.resetAll.In(x, y)) {
        for (int i : ShownPrefs())
            if (Changed(kPrefs[i])) SetPref(kPrefs[i], kPrefs[i].def);
        return;
    }
    if (g_prefs.search.empty())
        for (size_t i = 0; i < g_prefs.links.size(); ++i)
            if (g_prefs.links[i].In(x, y)) {
                g_prefs.linkFocus = (int)i;
                OpenPreferenceLink(i);
                return;
            }
    if (!g_prefs.list.In(x, y)) return;
    for (const PrefRow& row : g_prefs.rows) {
        const Pref& t = kPrefs[row.pref];
        double v = t.get();
        if (row.toggle.In(x, y)) SetPref(t, v != 0 ? 0 : 1);
        else if (row.less.In(x, y)) SetPref(t, v - t.step);
        else if (row.more.In(x, y)) SetPref(t, v + t.step);
        else if (row.reset.w > 0 && row.reset.In(x, y)) SetPref(t, t.def);
        else continue;
        auto shown = ShownPrefs();
        g_prefs.focus = (int)(std::find(shown.begin(), shown.end(), row.pref) - shown.begin());
        g_prefs.linkFocus = -1;
        return;
    }
}

// Typing searches; Up and Down pick a setting, Left and Right step it (Space flips an on/off one),
// Delete resets it, Escape clears the search and then closes. Tab visits enabled actions; Enter
// or Space runs the focused action, and Up/Down returns to the settings.
void PrefsKey(const SDL_KeyboardEvent& e) {
    auto shown = ShownPrefs();
    int& focus = g_prefs.focus;
    const Pref* t = focus >= 0 && focus < (int)shown.size() ? &kPrefs[shown[focus]] : nullptr;
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
        case SDLK_PAGEUP: g_prefs.scroll -= g_prefs.list.h; break;
        case SDLK_PAGEDOWN: g_prefs.scroll += g_prefs.list.h; break;
        default: break;
    }
    ClampPrefsScroll();
    g.redraw = true;
}

void PrefsText(const char* text) {
    if (g_prefs.focus >= 0 && !strcmp(text, " ")) return;  // Space flips the focused setting instead
    g_prefs.search += compat::Widen(text);
    g_prefs.scroll = 0;
    g_prefs.focus = -1;
    g.redraw = true;
}

void PrefsWheel(float, float, float dy) {
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
    float right = g_prefs.list.x;
    for (const auto& button : g_prefs.links) {
        active &= button.w > 0 && button.x >= right && button.R() <= g_prefs.panel.R() &&
                  button.y >= g_prefs.list.B() && button.B() <= g_prefs.resetAll.y;
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
