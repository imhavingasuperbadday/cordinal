// The Tweaks registry (app_tweaks.h) and the Preferences window built from it (File ▸ Preferences,
// Ctrl+,): a page per area of the registry, in captioned groups, plus All settings; one search across
// every page; and each page a scrolling sheet of its settings under headings, each with its help, its
// default, its own control and a reset. A change applies at once and goes through the same menu command
// the setting already has, so the window and the menus can never disagree about what a change does.
// Pages also link to the windows that hold the rest of their area's settings.
#include "app.h"

#include "metadata_fetch.h"
#include "peaks.h"
#include "musicbrainz.h"
#include "discord.h"
#include "listenbrainz.h"

namespace app {

namespace {

const wchar_t* const kRgModes[] = {L"Album", L"Track", L"By playback order"};
const wchar_t* const kOffOn[] = {L"Off", L"On"};
const wchar_t* const kLoopNames[] = {L"Ignore loop points", L"Play the loop once, then fade",
                                     L"Play the loop twice, then fade", L"Play the loop three times, then fade"};
const wchar_t* const kReverbNames[] = {L"Off", L"Room", L"Hall", L"Plate"};
const wchar_t* const kBitcrushBitNames[] = {L"2 bits", L"3 bits", L"4 bits", L"5 bits", L"6 bits", L"7 bits",
                                            L"8 bits", L"9 bits", L"10 bits", L"11 bits", L"12 bits", L"13 bits",
                                            L"14 bits", L"15 bits", L"16 bits"};
const wchar_t* const kBitcrushRateNames[] = {L"2 kHz", L"4 kHz", L"6 kHz", L"8 kHz", L"11 kHz", L"16 kHz",
                                             L"22 kHz", L"32 kHz", L"48 kHz"};
static_assert(std::size(kBitcrushBitNames) == kBitcrushMaxBits - kBitcrushMinBits + 1 &&
              std::size(kBitcrushRateNames) == std::size(kBitcrushRates));
const wchar_t* const kUndoNames[] = {L"1 day", L"3 days", L"1 week", L"7 weeks"};
const wchar_t* const kSavingNames[] = {L"Ask before saving", L"Save automatically", L"Manual only (don't ask)"};
const wchar_t* const kNewMusicNames[] = {L"Add automatically", L"Review in Inbox"};
const wchar_t* const kCoverSourceNames[] = {L"Deezer", L"iTunes", L"MusicBrainz"};
const wchar_t* const kCoverSaveNames[] = {L"Save as cover.jpg in the album folder", L"Embed in the files' tags",
                                          L"Show in cordinal only"};
const wchar_t* const kBandWaveSizes[] = {L"Use layout size", L"Slim (26 px)", L"Medium (44 px)", L"Tall (72 px)",
                                       L"Extra tall (108 px)", L"Largest (144 px)"};
static_assert(std::size(kBandWaveSizes) == std::size(ui::kBandWaveHeights));
// The seek bar's look: a plain progress bar first, then the SeekStyle values (peaks.h) one up.
const wchar_t* const kSeekBarNames[] = {L"Progress bar", L"Waveform", L"Moodbar", L"Mood waveform"};
// Waveform reflection's shapes. Mirrored is WaveReflection on; with it off, WaveFullHeight (default on)
// picks Full height over Upper half, so a profile from before the choice opens in Full height.
const wchar_t* const kWaveShapeNames[] = {L"Upper half", L"Mirrored", L"Full height"};
const wchar_t* const kSeekShowsNames[] = {L"Whole song", L"Live audio", L"Live audio with progress"};
const wchar_t* const kMoodColorNames[] = {L"Theme", L"Vivid", L"Spectrum", L"Warm", L"Cool",
                                          L"Colour-blind friendly", L"Single colour"};
const wchar_t* const kMoodWaveColorNames[] = {L"Same as moodbar", L"Theme", L"Vivid", L"Spectrum", L"Warm", L"Cool",
                                              L"Colour-blind friendly", L"Single colour"};
const wchar_t* const kWaveColorNames[] = {L"Theme", L"Red", L"Orange", L"Yellow", L"Green", L"Teal",
                                          L"Blue", L"Purple", L"Pink", L"Grey"};
const wchar_t* const kWaveRestNames[] = {L"Theme grey", L"Faded waveform colour"};
static_assert(std::size(kSeekBarNames) == kSeekStyles + 1 && std::size(kMoodColorNames) == kMoodColorSchemes &&
              std::size(kMoodWaveColorNames) == kMoodColorSchemes + 1 &&
              std::size(kWaveColorNames) == kWaveColorChoices && std::size(kWaveRestNames) == kWaveRestChoices);
static_assert(std::size(kLoopNames) == 4 && std::size(kReverbNames) == (size_t)ReverbPreset::Count &&
              std::size(kUndoNames) == std::size(kConvertMemoryDays) &&
              std::size(kSavingNames) == (size_t)PreferenceSaving::Count &&
              std::size(kCoverSourceNames) == online::kCoverSources);

// Choice lists for the non-tweak features below, in the same order the menus offer them.
const wchar_t* const kSpeedNames[] = {L"0.5x", L"0.75x", L"0.9x", L"Normal (1x)", L"1.1x", L"1.25x", L"1.5x", L"2x"};
const wchar_t* const kTempoNames[] = {L"0.75x", L"0.9x", L"Normal (1x)", L"1.1x", L"1.25x", L"1.5x", L"1.75x", L"2x"};
const wchar_t* const kReverbMixNames[] = {L"25%", L"40%", L"55%", L"75%"};
const wchar_t* const kWaveStyleNames[] = {L"Bars", L"Fountain bars", L"Dark Fountain (DELTARUNE)"};
const wchar_t* const kSkinNames[] = {L"Capacala", L"Butterflies", L"Letters", L"Notes", L"None"};
const wchar_t* const kOpacityNames[] = {L"20%", L"40%", L"60%", L"80%", L"100%"};
const wchar_t* const kFoobarViewNames[] = {L"By folder structure", L"By artist/album", L"By album", L"By genre"};
const wchar_t* const kDockNames[] = {L"Top", L"Bottom", L"Left", L"Right"};
const wchar_t* const kWaveSizeNames[] = {L"Slim", L"Medium", L"Tall"};
const wchar_t* const kDiscordNames[] = {L"Off", L"Song", L"Album"};
static_assert(std::size(kSpeedNames) == std::size(kSpeeds) && std::size(kTempoNames) == std::size(kTempos) &&
              std::size(kReverbMixNames) == std::size(kReverbMixes) && std::size(kWaveStyleNames) == 3 &&
              std::size(kSkinNames) == 5 && std::size(kOpacityNames) == 5 && std::size(kFoobarViewNames) == 4 &&
              std::size(kDockNames) == (size_t)Dock::Count && std::size(kWaveSizeNames) == 3 &&
              std::size(kDiscordNames) == discord::kShowChoices);
template <size_t N>
int IndexOf(const double (&values)[N], double value) {
    for (size_t i = 0; i < N; ++i)
        if (std::fabs(values[i] - value) < 0.001) return (int)i;
    return -1;
}
template <size_t N>
int IndexOf(const float (&values)[N], float value) {
    for (size_t i = 0; i < N; ++i)
        if (std::fabs((double)values[i] - (double)value) < 0.001) return (int)i;
    return -1;
}
template <size_t N>
int IndexOf(const int (&values)[N], int value) {
    for (size_t i = 0; i < N; ++i)
        if (values[i] == value) return (int)i;
    return -1;
}
int OpacityIndex(int opacity) {
    return IndexOf(ApolloOpacityTable, opacity);
}

const wchar_t* const kCrossfadeNames[] = {L"Off", L"1 second", L"2 seconds", L"3 seconds", L"5 seconds", L"8 seconds"};
const wchar_t* const kVolumeCurveNames[] = {L"Natural", L"Linear", L"Cubic", L"Decibels (60 dB range)"};
static_assert(std::size(kCrossfadeNames) == std::size(kCrossfadeSeconds) && std::size(kVolumeCurveNames) == kVolumeCurves);

// The length applies from the next track end; the album choice from the next track chosen, which
// is re-asked now so the track already lined up follows it too.
void ApplyCrossfade() {
    g.engine.SetCrossfade(kCrossfadeSeconds[std::clamp(g.crossfade, 0, (int)std::size(kCrossfadeSeconds) - 1)]);
    RefreshNext();
}

// Grouped as the window shows them. Keys are permanent: renaming one would orphan saved values.
const Tweak kTweaks[] = {
    {"playback.resume", L"Player", L"ResumeOn", L"Resume where you left off",
     L"When cordinal starts, offer to continue the track that was playing when it closed.", TweakType::Bool, 0, 1, 1,
     [] { return (double)g.resumeOn; }, [](double v) { g.resumeOn = v != 0; }, IDM_RESUME},
    {"playback.resume_in_song", L"Player", L"ResumeInSong", L"Reopen in the last song",
     L"Cordinal starts with the last track loaded and paused at the point it stopped, instead of waiting for Continue.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.resumeInSong; }, [](double v) { g.resumeInSong = v != 0; }, 0,
     [] { ReopenLastTrack(); }},
    {"playback.resume_play", L"Player", L"ResumePlay", L"Start it playing on open",
     L"The track reopened from the last session starts playing as soon as cordinal opens, instead of waiting paused.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.resumePlay; }, [](double v) { g.resumePlay = v != 0; }, 0,
     [] { ReopenLastTrack(); }},
    {"playback.crossfade", L"Player", L"Crossfade", L"Crossfade between tracks",
     L"When one track runs into the next on its own, the end of the first fades out as the next fades in. "
     L"A track you start yourself, a radio stream and a section loop still change at once.",
     TweakType::Choice, 0, (float)std::size(kCrossfadeNames) - 1, 0, [] { return (double)g.crossfade; },
     [](double v) { g.crossfade = (int)v; }, 0, ApplyCrossfade, kCrossfadeNames},
    {"playback.crossfade_albums", L"Player", L"CrossfadeAlbums", L"Crossfade within an album",
     L"Off keeps the tracks of one album gapless, so live and continuous albums flow as recorded; the crossfade "
     L"then only joins different albums.", TweakType::Bool, 0, 1, 0, [] { return (double)g.crossfadeAlbums; },
     [](double v) { g.crossfadeAlbums = v != 0; }, 0, ApplyCrossfade},
    {"playback.game_loops", L"Player", L"Loops", L"Game music loops",
     L"How often a game soundtrack rip with loop points plays its loop before fading out.", TweakType::Choice, 0, 3, 2,
     [] { return (double)g.loops; }, [](double v) { g.loops = (int)v; }, IDM_LOOPS_BASE, nullptr, kLoopNames,
     nullptr, kTweakRadio},
    {"playback.skip_aware_shuffle", L"Player", L"SkipAwareShuffle", L"Skip-aware shuffle",
     L"Shuffle favors tracks you haven't repeatedly skipped in their first 30 seconds. A skip while flipping through "
     L"songs counts for less, and one just before you stop doesn't count.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.skipAwareShuffle; }, [](double v) { g.skipAwareShuffle = v != 0; }, IDM_SKIP_SHUFFLE},
    {"playback.radio", L"Player", L"Radio", L"Keep playing like this",
     L"Keeps choosing from the playing playlist, favoring what you tend to play after the current track.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.radio; }, [](double v) { g.radio = v != 0; }, IDM_RADIO},
    {"playback.taste", L"Player", L"Taste", L"Choose by taste",
     L"Track shuffle and Keep playing like this follow what you finish, rate, replay and come back to. Skips are judged by "
     L"their moment, and artists by how often you finish them, not how many of their songs you have.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.taste; }, [](double v) { g.taste = v != 0; }, IDM_TASTE},
    {"output.exclusive", L"Player", L"Exclusive", L"Exclusive output",
     L"Each track plays at its own rate, bit-perfect at 100% volume, and other programs can't play sound meanwhile.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.exclusive; }, [](double v) { g.exclusive = v != 0; },
     IDM_OUTPUT_SHARED, nullptr, nullptr, nullptr, kTweakRadio},
    {"output.software_volume", L"Player", L"SoftwareVolume", L"Volume control in cordinal",
     L"Off sends the music at full level and leaves the volume to Windows or your amplifier; cordinal's slider "
     L"rests and only mute still works. With Exclusive output, that keeps playback bit-perfect at any level.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.softwareVolume; }, [](double v) { g.softwareVolume = v != 0; },
     0, [] { SetVolumePercent(g.volumePercent); }},
    {"output.volume_curve", L"Player", L"VolumeCurve", L"Volume curve",
     L"How the slider's travel turns into loudness. Natural suits most listening; Linear is the plain level, "
     L"which bunches loudness at the bottom; Cubic gives finer steps when quiet; Decibels spreads 60 dB evenly.",
     TweakType::Choice, 0, kVolumeCurves - 1, 0, [] { return (double)g.volumeCurve; },
     [](double v) { g.volumeCurve = (int)v; }, 0, [] { SetVolumePercent(g.volumePercent); }, kVolumeCurveNames},
    {"replaygain.enabled", L"ReplayGain", L"Enabled", L"ReplayGain",
     L"Plays every track at a similar loudness, using measured or tagged ReplayGain values.", TweakType::Bool, 0, 1, 1,
     [] { return (double)g.rgOn; }, [](double v) { g.rgOn = v != 0; }, IDM_RG_ENABLE, nullptr, nullptr, nullptr,
     kTweakLegacyLoad},
    {"replaygain.mode", L"ReplayGain", L"SourceMode", L"Level by",
     L"Album preserves album relationships; track levels songs individually. Playback order uses track for track shuffle.",
     TweakType::Choice, 0, 2, 0, [] { return (double)g.rgMode; },
     [](double v) { g.rgMode = (ReplayGainMode)(int)v; }, 0, [] { RefreshGains(); }, kRgModes},
    {"replaygain.keep_loud", L"ReplayGain", L"KeepLoud", L"Loud songs keep their loudness",
     L"0 evens every track out; higher values turn loud songs down only part of the way.", TweakType::Float, 0, 0.9f,
     0.4f, [] { return (double)g.rgKeep; }, [](double v) { g.rgKeep = (float)v; }, 0, [] { RefreshGains(); }},
    {"replaygain.preamp", L"ReplayGain", L"Preamp", L"Preamp",
     L"Added to every track's ReplayGain.", TweakType::Float, -24, 24, 0, [] { return (double)g.rgPreamp; },
     [](double v) { g.rgPreamp = (float)v; }, 0, [] { RefreshGains(); }, nullptr, L"dB"},
    {"replaygain.preamp_without_info", L"ReplayGain", L"PreampWithoutInfo", L"Gain for unmeasured tracks",
     L"Used for tracks with no ReplayGain yet, limited so their peaks don't clip.", TweakType::Float, -24, 24, 0,
     [] { return (double)g.rgPreampNoInfo; }, [](double v) { g.rgPreampNoInfo = (float)v; }, 0,
     [] { RefreshGains(); }, nullptr, L"dB"},
    {"replaygain.auto_measure", L"ReplayGain", L"AutoMeasure", L"Measure new tracks automatically",
     L"Measures the loudness of new tracks in the background, giving way to playback and to your own scans.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.rgAutoScan; }, [](double v) { g.rgAutoScan = v != 0; },
     IDM_RG_AUTO_SCAN},
    {"replaygain.estimate", L"ReplayGain", L"EstimateUnmeasured", L"Estimate unmeasured tracks as they start",
     L"Guesses the loudness of a track with no ReplayGain from its first moments while it plays.", TweakType::Bool, 0,
     1, 1, [] { return (double)g.rgEstimate; }, [](double v) { g.rgEstimate = v != 0; }, IDM_RG_ESTIMATE},
    {"effects.reverb", L"Effects", L"Reverb", L"Reverb", L"A room around the music.", TweakType::Choice, 0, 3, 0,
     [] { return (double)g.reverb.preset; }, [](double v) { g.reverb.preset = (ReverbPreset)(int)v; },
     IDM_REVERB_BASE, nullptr, kReverbNames, nullptr, kTweakRadio},
    {"effects.reverb_amount", L"Effects", L"ReverbAmount", L"Reverb amount",
     L"How much of the room is mixed in, from 0 (none) to 1 (all of it).", TweakType::Float, 0, 1, 0.4f,
     [] { return (double)g.reverb.mix; }, [](double v) { g.reverb.mix = (float)v; }, 0, ApplyEffects},
    {"effects.loudness_compensation", L"Effects", L"LoudnessCompensation", L"Loudness compensation at low volume",
     L"Adds bass and treble as the volume goes down, so quiet listening doesn't sound thin.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.loudnessComp; }, [](double v) { g.loudnessComp = v != 0; }, IDM_LOUDNESS_COMP},
    {"effects.night_mode", L"Effects", L"NightMode", L"Night mode",
     L"Slowly lifts quiet passages and softens loud ones, for listening at night.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.nightMode; }, [](double v) { g.nightMode = v != 0; }, IDM_NIGHT_MODE},
    {"effects.bitcrush", L"Effects", L"Bitcrush", L"Bitcrush",
     L"Lo-fi crunch: fewer levels and a lower sample rate, like an old sampler or game console.", TweakType::Bool,
     0, 1, 0, [] { return (double)g.bitcrush.enabled; }, [](double v) { g.bitcrush.enabled = v != 0; },
     IDM_BITCRUSH},
    {"effects.bitcrush_bits", L"Effects", L"BitcrushBits", L"Bitcrush depth",
     L"How many bits each sample keeps. Fewer is harsher and buzzier; at 4 or fewer, quiet passages drop out.",
     TweakType::Choice, 0, (float)std::size(kBitcrushBitNames) - 1, 8 - kBitcrushMinBits,
     [] { return (double)(g.bitcrush.bits - kBitcrushMinBits); },
     [](double v) { g.bitcrush.bits = (int)v + kBitcrushMinBits; }, 0, ApplyEffects, kBitcrushBitNames},
    {"effects.bitcrush_rate", L"Effects", L"BitcrushRate", L"Bitcrush sample rate",
     L"The rate the sound is held at. Lower rates lose treble and add metallic aliasing.", TweakType::Choice, 0,
     (float)std::size(kBitcrushRateNames) - 1, kBitcrushDefaultRate,
     [] { return (double)BitcrushRateIndex(g.bitcrush.rate); },
     [](double v) { g.bitcrush.rate = kBitcrushRates[std::clamp((int)v, 0, (int)std::size(kBitcrushRates) - 1)]; }, 0,
     ApplyEffects, kBitcrushRateNames},
    {"effects.bitcrush_mix", L"Effects", L"BitcrushMix", L"Bitcrush mix",
     L"How much of the crushed sound replaces the clean one, from 0 (none) to 1 (all of it).", TweakType::Float, 0,
     1, 1, [] { return (double)g.bitcrush.mix; }, [](double v) { g.bitcrush.mix = (float)v; }, 0, ApplyEffects},
    // Seek bar and Seek bar shows each stand for two of the [Window] keys earlier versions offered as
    // separate switches (Waveform and SeekStyle; LiveWaveform and FullLiveWaveform). Those keys stay
    // as they were, so profiles move between versions unchanged; LoadTweaks and SaveTweaks keep them.
    {"view.seek_bar", L"Window", L"SeekStyle", L"Seek bar",
     L"How the seek bar looks. Moodbar colours each moment by its sound: one way for bass, the other for "
     L"treble, brighter when louder. Mood waveform keeps the waveform's shape in those colours.",
     TweakType::Choice, 0, 3, 1, [] { return g.waveform ? (double)g.seekStyle + 1 : 0.0; },
     [](double v) { g.waveform = v != 0; if (v != 0) g.seekStyle = (int)v - 1; }, 0,
     [] { WaveformTrackChanged(true); WaveformTick(); if (g.seek) InvalidateRect(g.seek, nullptr, FALSE); },
     kSeekBarNames},
    {"view.seek_shows", L"Window", L"LiveWaveform", L"Seek bar shows",
     L"The whole song, or a moving picture of the audio being heard; seeking still works either way, and Radio "
     L"always shows live audio. With progress, what has played is lit and the rest dimmed.",
     TweakType::Choice, 0, 2, 0, [] { return !g.liveWaveform ? 0.0 : g.fullLiveWaveform ? 1.0 : 2.0; },
     [](double v) { g.liveWaveform = v != 0; if (v != 0) g.fullLiveWaveform = v == 1; }, 0,
     [] { WaveformTrackChanged(true); WaveformTick(); if (g.seek) InvalidateRect(g.seek, nullptr, FALSE); },
     kSeekShowsNames},
    {"view.wave_60fps", L"Window", L"Wave60Fps", L"60 frames a second",
     L"Draws the live waveform and the seek bar's glow 60 times a second instead of the default 30, for "
     L"smoother motion. Uses a little more CPU.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.wave60Fps; }, [](double v) { g.wave60Fps = v != 0; }, 0,
     [] { WaveformTick(); }},
    {"view.seek_glow", L"Window", L"SeekGlow", L"Seek bar glow",
     L"A soft halo of the seek bar's colours around it that pulses with the beat, brighter where the song is loud. "
     L"Every look, whole song or live.", TweakType::Bool, 0, 1, 1, [] { return (double)g.seekGlow; }, [](double v) { g.seekGlow = v != 0; },
     0, [] { if (g.seek) InvalidateRect(g.seek, nullptr, FALSE); }},
    {"view.pause_fade", L"Window", L"PauseFade", L"Fade the waveform when pausing",
     L"The waveform's colour fades smoothly between playing and paused, instead of switching at once. Paused "
     L"turns amber, or the text colour when your waveform colour is itself amber or yellow.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.pauseFade; }, [](double v) { g.pauseFade = v != 0; },
     0, [] { WaveformTick(); if (g.seek) InvalidateRect(g.seek, nullptr, FALSE); }},
    {"view.live_fountain", L"Window", L"LiveFountain", L"Live audio for Dark Fountain",
     L"In the DELTARUNE theme, the selected fountain style follows the audio being heard, including Radio. "
     L"Song seeking still works.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.liveFountain; }, [](double v) { g.liveFountain = v != 0; }, 0,
     [] { WaveformTrackChanged(true); WaveformTick(); }},
    {"view.list_wind", L"Window", L"ListWind", L"Wind behind the track list",
     L"Soft ribbons drift across the right of the track list while music plays, curling, swelling and "
     L"gusting with it. Bass makes them wide and slow, treble short and quick. Off while Windows "
     L"animations are turned off.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.listWind; }, [](double v) { g.listWind = v != 0; }, 0,
     [] { ListWindChanged(); }},
    {"view.band_wave_size", L"Window", L"BandWaveSize", L"Waveform height in the top or bottom bar",
     L"Sets the waveform's height only in the normal top or bottom player bar. "
     L"Big, compact and side players keep their own sizes.", TweakType::Choice, 0, 5, 0,
     [] { return (double)g.bandWaveSize; }, [](double v) { g.bandWaveSize = (int)v; }, 0,
     [] { Layout(); WaveformTrackChanged(true); WaveformTick(); }, kBandWaveSizes},
    {"view.wave_color", L"Window", L"WaveColor", L"Waveform colour",
     L"The waveform's played colour, kept bright enough to read on the player. The moodbar's Theme, Vivid and "
     L"Single colour schemes turn from it too. Paused and warnings keep their own colours.",
     TweakType::Choice, 0, kWaveColorChoices - 1, 0, [] { return (double)g.waveColor; },
     [](double v) { g.waveColor = (int)v; }, 0,
     [] { WaveformTick(); if (g.seek) InvalidateRect(g.seek, nullptr, FALSE); if (g.list) InvalidateRect(g.list, nullptr, FALSE); },
     kWaveColorNames},
    {"view.wave_rest_color", L"Window", L"WaveRestColor", L"Waveform unplayed part",
     L"What is still to play: the theme's grey, or the waveform's own colour faded toward the background.",
     TweakType::Choice, 0, kWaveRestChoices - 1, 0, [] { return (double)g.waveRestColor; },
     [](double v) { g.waveRestColor = (int)v; }, 0,
     [] { WaveformTick(); if (g.seek) InvalidateRect(g.seek, nullptr, FALSE); }, kWaveRestNames},
    {"view.mood_colors", L"Window", L"MoodColors", L"Moodbar colours",
     L"The moodbar's colours: turned from the waveform colour, the whole colour wheel, red for bass, green "
     L"for mids and blue for treble, warm or cool shades, blue to orange (easy to tell apart with colour "
     L"blindness), or one colour that stands out more for treble.",
     TweakType::Choice, 0, kMoodColorSchemes - 1, 0, [] { return (double)g.moodColors; },
     [](double v) { g.moodColors = (int)v; }, 0,
     [] { if (g.seek) InvalidateRect(g.seek, nullptr, FALSE); if (g.list) InvalidateRect(g.list, nullptr, FALSE); },
     kMoodColorNames},
    {"view.mood_wave_colors", L"Window", L"MoodWaveColors", L"Mood waveform colours",
     L"The Mood waveform's colours: the moodbar's, or a scheme of its own.", TweakType::Choice, 0,
     kMoodColorSchemes, 0, [] { return (double)g.moodWaveColors; }, [](double v) { g.moodWaveColors = (int)v; }, 0,
     [] { if (g.seek) InvalidateRect(g.seek, nullptr, FALSE); }, kMoodWaveColorNames},
    {"view.smooth_seek", L"Window", L"SmoothSeek", L"Smooth seek bar",
     L"Rounds off the whole song's outline and blends its moodbar colours into each other. "
     L"The live waveform keeps its detail.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.smoothSeek; }, [](double v) { g.smoothSeek = v != 0; }, 0,
     [] { if (g.seek) InvalidateRect(g.seek, nullptr, FALSE); }},
    {"view.wave_reflection", L"Window", L"WaveReflection", L"Waveform reflection",
     L"Mirrored reflects the seek bar below its center line, dimmer. Upper half stops at the center line; "
     L"Full height mirrors the waveforms as bright below as above (the Moodbar stripe keeps the upper "
     L"half).", TweakType::Choice, 0, 2, 1,
     [] { return g.waveReflection ? 1.0 : g.waveFullHeight ? 2.0 : 0.0; },
     [](double v) { g.waveReflection = v == 1; if (v != 1) g.waveFullHeight = v == 2; }, 0,
     [] { if (g.seek) InvalidateRect(g.seek, nullptr, FALSE); }, kWaveShapeNames},
    {"view.group_albums", L"Window", L"GroupAlbums", L"Group tracks by album",
     L"The track list shows a header with the cover above each album's tracks.", TweakType::Bool, 0, 1, 1,
     [] { return (double)g.groupAlbums; }, [](double v) { g.groupAlbums = v != 0; }, IDM_GROUP_ALBUMS},
    {"view.now_playing_album", L"Window", L"NowPlayingAlbumOnly", L"Now Playing shows the playing album",
     L"While a song with an album plays, the Now Playing tab lists just that album. Typing a search "
     L"looks through the whole list again; songs without an album always show the whole list.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.npAlbumOnly; }, [](double v) { g.npAlbumOnly = v != 0; }, 0,
     [] { if (g.hwnd && !g.playlists.empty()) RebuildView(true); }},
    {"view.group_auto_tabs", L"Window", L"GroupAutoTabs", L"One tab for auto-playlists",
     L"Auto-playlists such as Recently added and Top rated share one tab; click it again to choose "
     L"another. Off gives each its own tab.", TweakType::Bool, 0, 1, 1,
     [] { return (double)g.groupAutoTabs; }, [](double v) { g.groupAutoTabs = v != 0; }, 0, [] { RefreshTabs(); }},
    {"view.taskbar_progress", L"Window", L"TaskbarProgress", L"Show progress on the taskbar",
     L"The taskbar button fills as the track plays.", TweakType::Bool, 0, 1, 1,
     [] { return (double)g.taskbarProgress; }, [](double v) { g.taskbarProgress = v != 0; }, IDM_TASKBAR_PROGRESS},
    {"view.focus_boxes", L"Window", L"ShowFocusBoxes", L"Show focus boxes",
     L"Show outlines around the focused control for keyboard navigation.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.showFocusBoxes; }, [](double v) { g.showFocusBoxes = v != 0; }, 0, [] {
         if (g.hwnd) {
             SendMessageW(g.hwnd, WM_CHANGEUISTATE,
                          MAKEWPARAM(g.showFocusBoxes ? UIS_CLEAR : UIS_SET, UISF_HIDEFOCUS), 0);
             RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
         }
         if (g.eqWnd) RedrawWindow(g.eqWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
     }},
    {"view.smooth_scroll", L"Window", L"SmoothScroll", L"Smooth scrolling",
     L"The library eases toward where the mouse wheel or Page Up and Page Down take it, instead of "
     L"jumping. Scrollbar drags and keyboard focus still move at once.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.smoothScroll; }, [](double v) { g.smoothScroll = v != 0; }, 0,
     [] { LibViewSmoothScrollChanged(); }},
    {"view.gpu_drawing", L"Window", L"GpuDrawing", L"Draw with the graphics card",
     L"Draws the library with the graphics card instead of the processor. Off uses less memory; turn it on "
     L"if scrolling is slow on a large, high-resolution screen.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.gpuDrawing; }, [](double v) { g.gpuDrawing = v != 0; }, 0,
     [] {
         UpdateViewMemory();  // the next paint creates the other kind of target
         if (g.hwnd) RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
     }},
    {"view.always_on_top", L"Window", L"AlwaysOnTop", L"Always on top",
     L"Keeps cordinal's window above other windows.", TweakType::Bool, 0, 1, 0, [] { return (double)g.alwaysOnTop; },
     [](double v) { g.alwaysOnTop = v != 0; }, IDM_ALWAYS_ON_TOP},
    {"view.sound_effects", L"Player", L"SoundEffects", L"Sound effects",
     L"Small, quiet sounds for things that otherwise happen silently, such as saving tags or dropping files.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.soundEffects; }, [](double v) { g.soundEffects = v != 0; },
     IDM_SOUND_EFFECTS},
    {"view.robotic_sounds", L"Window", L"RoboticSounds", L"Robotic sounds in the DELTARUNE theme",
     L"The theme's sounds come bitcrushed, in Chapter 2's Cyber World voice, and messages are spoken in Queen's blip.",
     TweakType::Bool, 0, 1, 1, [] { return (double)RoboticSounds(); }, [](double v) { SetRoboticSounds(v != 0); },
     IDM_THEME_ROBOTIC},
    {"startup.with_windows", nullptr, nullptr, L"Start with Windows", L"Opens cordinal when you sign in to Windows.",
     TweakType::Bool, 0, 1, 0, [] { return (double)StartWithWindows(); }, [](double) {}, IDM_START_WITH_WINDOWS,
     nullptr, nullptr, nullptr, kTweakOutside},
    {"startup.saving", L"Suggestions", L"SavingMode", L"Preference saving",
     L"Whether a new window size, theme, library view, browser or layout becomes how cordinal opens: asked about once "
     L"it settles, saved as you go, or only through Startup defaults.", TweakType::Choice, 0, 2, 0,
     [] { return (double)g.preferenceSaving; }, [](double v) { g.preferenceSaving = (PreferenceSaving)(int)v; },
     IDM_PREFERENCE_ASK, nullptr, kSavingNames, nullptr, kTweakRadio | kTweakLegacyLoad},
    {"convert.recycle", L"Convert", L"Recycle", L"Send converted originals to the Recycle Bin",
     L"After converting to .cord, the original files go to the Recycle Bin instead of staying where they are.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.convertRecycle; }, [](double v) { g.convertRecycle = v != 0; },
     IDM_CONVERT_RECYCLE},
    {"convert.show_originals", L"Convert", L"ShowOriginals", L"Show kept originals in the library",
     L"Originals kept after a conversion appear in the library beside their .cord copies.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.convertShowOriginals; }, [](double v) { g.convertShowOriginals = v != 0; },
     IDM_CONVERT_SHOW_ORIGINALS},
    {"convert.undo_days", L"Convert", L"Memory", L"Conversions can be undone for",
     L"How long cordinal remembers a conversion so it can put the originals back.", TweakType::Choice, 0, 3, 3,
     [] { return (double)g.convertMemory; }, [](double v) { g.convertMemory = (uint8_t)v; }, IDM_CONVERT_MEMORY_BASE,
     nullptr, kUndoNames, nullptr, kTweakRadio},
    {"scrobbling.paused", L"Scrobbling", L"Paused", L"Pause scrobbling",
     L"Stops collecting and sending listens to Last.fm, ListenBrainz and Libre.fm, keeping connections and queued listens.",
     TweakType::Bool, 0, 1, 0, [] { return (double)listenbrainz::Paused(); },
     [](double v) { listenbrainz::SetPaused(v != 0); }, 0,
     [] { ShowMessage(listenbrainz::Paused() ? L"Scrobbling is paused." : L"Scrobbling can send to connected services."); }},
    {"scrobbling.now_playing", L"Scrobbling", L"NowPlaying", L"Send now-playing updates",
     L"Shows the current song on connected services. Turning this off still submits completed listens.",
     TweakType::Bool, 0, 1, 1, [] { return (double)listenbrainz::SendNowPlaying(); },
     [](double v) { listenbrainz::SetSendNowPlaying(v != 0); }, 0,
     [] { ShowMessage(listenbrainz::SendNowPlaying() ? L"Now-playing updates are on." : L"Now-playing updates are off."); }},
    {"scrobbling.discord", L"Scrobbling", L"Discord", L"Discord status",
     L"While the Discord app runs on this computer, your status shows the song playing or the album, with how far "
     L"along it is. Nothing is shown while paused. Discord shows it only while its own Activity Privacy setting "
     L"shares your activity. The picture is the cover cordinal shows: a cover chosen from the online results keeps "
     L"the address it came from, and a cover chosen from a file has none, so the album's cover from the sources "
     L"under Online stands in for it. To show those too, set DiscordCoverUpload in the Scrobbling section of "
     L"settings.ini to a command that uploads the image you hand it and prints its https address.",
     TweakType::Choice, 0, discord::kShowChoices - 1, 0, [] { return (double)discord::Mode(); },
     [](double v) { discord::SetMode((discord::Show)v); }, 0,
     [] {
         static const wchar_t* const said[] = {L"Discord status is off.", L"Discord shows the song playing.",
                                               L"Discord shows the album playing."};
         ShowMessage(said[(int)discord::Mode()]);
     },
     kDiscordNames},
    {"library.new_music", L"Library", L"Inbox", L"New music from library folders",
     L"Add music automatically or send it to the Inbox; existing Inbox tracks stay.",
     TweakType::Choice, 0, 1, 0, [] { return (double)g.inboxEnabled; }, [](double v) { g.inboxEnabled = v != 0; },
     0, [] { ShowMessage(g.inboxEnabled ? L"New music will wait in the Inbox." : L"New music will be added to the library."); },
     kNewMusicNames},
    {"library.musicbrainz", L"Library", L"MusicBrainz", L"Look up missing album editions online",
     L"Asks MusicBrainz for a release-group id when two local albums share an artist and base title and their tags "
     L"carry none. Results are cached; browsing and playback never wait for or require the network.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.musicBrainzLookup; },
     [](double v) { g.musicBrainzLookup = v != 0; musicbrainz::SetEnabled(g.musicBrainzLookup); }, 0,
     [] { musicbrainz::SetEnabled(g.musicBrainzLookup); }},
    // Online sources: each says exactly what it sends, and all are off until chosen.
    {"online.covers_deezer", L"Online", L"CoversDeezer", L"Covers from Deezer",
     L"Sends the album's artist and title to api.deezer.com "
     L"when you look for a cover, or for an album cover in the Discord status.", TweakType::Bool, 0, 1, 0,
     [] { return (double)online::Config().covers[0]; }, [](double v) { online::Config().covers[0] = v != 0; }, 0,
     OnlineSettingsChanged},
    {"online.covers_itunes", L"Online", L"CoversITunes", L"Covers from iTunes",
     L"Sends the album's artist and title to itunes.apple.com "
     L"when you look for a cover, or for an album cover in the Discord status.", TweakType::Bool, 0, 1, 0,
     [] { return (double)online::Config().covers[1]; }, [](double v) { online::Config().covers[1] = v != 0; }, 0,
     OnlineSettingsChanged},
    {"online.covers_musicbrainz", L"Online", L"CoversMusicBrainz", L"Covers from MusicBrainz",
     L"Sends the album's artist and title to musicbrainz.org, then the release it finds to coverartarchive.org, "
     L"when you look for a cover or for one in the Discord status.",
     TweakType::Bool, 0, 1, 0, [] { return (double)online::Config().covers[2]; },
     [](double v) { online::Config().covers[2] = v != 0; }, 0, OnlineSettingsChanged},
    {"online.cover_first", L"Online", L"CoverFirst", L"Preferred cover source",
     L"Its covers are listed first, and it wins when two covers are about as good.", TweakType::Choice, 0, 2, 0,
     [] { return (double)online::Config().coverFirst; }, [](double v) { online::Config().coverFirst = (int)v; }, 0,
     OnlineSettingsChanged, kCoverSourceNames},
    {"online.cover_save", L"Online", L"CoverSave", L"Save chosen covers",
     L"Shown in cordinal only, or as a folder file, no audio file changes. Embedding rewrites each track's tags; Undo last tag edit takes it back.",
     TweakType::Choice, 0, 2, 2, [] { return (double)online::Config().coverSave; },
     [](double v) { online::Config().coverSave = (int)v; }, 0, OnlineSettingsChanged, kCoverSaveNames},
    {"online.cover_overwrite", L"Online", L"CoverOverwrite", L"Replace an existing cover file",
     L"When off, an album folder that already has cover.jpg, folder.jpg or front.jpg keeps it.", TweakType::Bool, 0, 1,
     0, [] { return (double)online::Config().coverOverwrite; },
     [](double v) { online::Config().coverOverwrite = v != 0; }, 0, OnlineSettingsChanged},
    {"online.lyrics_lrclib", L"Online", L"LyricsLrcLib", L"Lyrics from LrcLib",
     L"Sends the song's artist, title, album and length to lrclib.net when it has no lyrics of its own.",
     TweakType::Bool, 0, 1, 0, [] { return (double)online::Config().lyrics[0]; },
     [](double v) { online::Config().lyrics[0] = v != 0; }, 0, LyricsSourcesChanged},
    {"online.lyrics_ovh", L"Online", L"LyricsOvh", L"Lyrics from lyrics.ovh",
     L"Sends the song's artist and title to api.lyrics.ovh when it has no lyrics of its own and LrcLib had none.",
     TweakType::Bool, 0, 1, 0, [] { return (double)online::Config().lyrics[1]; },
     [](double v) { online::Config().lyrics[1] = v != 0; }, 0, LyricsSourcesChanged},
    {"online.artist_info", L"Online", L"ArtistInfo", L"Artist info from MusicBrainz and Wikipedia",
     L"Sends the artist's name to musicbrainz.org, then asks wikidata.org and en.wikipedia.org for the article it names.",
     TweakType::Bool, 0, 1, 0, [] { return (double)online::Config().artistInfo; },
     [](double v) { online::Config().artistInfo = v != 0; }, 0, OnlineSettingsChanged},
    {"library.explorer_menu", L"Explorer", L"FolderMenu", L"Play folders from Explorer",
     L"Right-clicking a folder offers Play with cordinal and Add to cordinal; a folder plays its tracks like a "
     L"command line does.", TweakType::Bool, 0, 1, 1, [] { return (double)g.explorerMenu; },
     [](double v) { g.explorerMenu = v != 0; }, 0, ApplyExplorerMenu},
    {"library.web_start", L"Web", L"Start", L"Start the web server with cordinal",
     L"Turns on the web server whenever cordinal starts, so a paired phone or browser can play your library.",
     TweakType::Bool, 0, 1, 0, [] { return (double)WebStartsWithCordinal(); },
     [](double v) { SetWebStartsWithCordinal(v != 0); }, 0, ApplyWebStart},
    // Non-tweak features: session rates, sizes and menu-only windows, exposed as preferences so
    // every persistent choice is one home (Preferences). Speed, tempo and the reverb-mix preset
    // are session rates: their rows name the menu's own command and never reach the file (see
    // SaveTweaks). Everything else reuses the ini key its feature already writes.
    {"playback.speed", L"Player", L"Speed", L"Playback speed",
     L"Lasts for this session only: the next start is always normal speed, so a podcast's rate "
     L"cannot carry over to tomorrow's album. Pitch follows.",
     TweakType::Choice, 0, (float)std::size(kSpeedNames) - 1, 3,
     [] { return (double)std::max(0, IndexOf(kSpeeds, g.speed)); },
     [](double v) { if (int i = (int)v; i >= 0 && i < (int)std::size(kSpeedNames)) SetSpeed(kSpeeds[i]); },
     IDM_SPEED_BASE, nullptr, kSpeedNames, nullptr, kTweakRadio | kTweakLegacyLoad},
    {"playback.tempo", L"Player", L"Tempo", L"Playback tempo",
     L"Lasts for this session only, like speed. Pitch stays.",
     TweakType::Choice, 0, (float)std::size(kTempoNames) - 1, 2,
     [] { return (double)std::max(0, IndexOf(kTempos, g.tempo)); },
     [](double v) { if (int i = (int)v; i >= 0 && i < (int)std::size(kTempoNames)) SetTempo(kTempos[i]); },
     IDM_TEMPO_BASE, nullptr, kTempoNames, nullptr, kTweakRadio | kTweakLegacyLoad},
    {"playback.time_remaining", L"Player", L"TimeRemaining", L"Show the time remaining",
     L"The time beside the seek bar counts down to the end of the track instead of up from its start.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.timeRemaining; }, [](double v) { g.timeRemaining = v != 0; }, 0,
     UpdateStatus},
    {"effects.reverb_mix_choice", L"Effects", L"ReverbMixChoice", L"Reverb amount (preset)",
     L"The menu's preset amounts, as an alternative to dragging the slider: 25%, 40%, 55% or 75%. "
     L"The first preset at or below the slider is shown.",
     TweakType::Choice, 0, 3, 1,
     [] { return (double)std::max(0, IndexOf(kReverbMixes, g.reverb.mix)); },
     [](double v) { if (int i = (int)v; i >= 0 && i <= 3) OnCommand(IDM_REVERB_MIX_BASE + i, 0, nullptr); },
     IDM_REVERB_MIX_BASE, nullptr, kReverbMixNames, nullptr, kTweakRadio | kTweakLegacyLoad},
    {"library.cover_size", L"Library", L"CoverSize", L"Album cover size",
     L"The covers' size in the Albums and Artists views, in pixels. Ctrl+wheel over the view also changes it. "
     L"Startup defaults offers to keep it for the next start.",
     TweakType::Float, 80, 300, 160, [] { return (double)g.coverSize; },
     [](double v) { LibViewSetCoverSize((int)std::lround(v)); }, 0, nullptr, nullptr, L"px"},
    {"library.cover_gap", L"Library", L"CoverGap", L"Album cover spacing",
     L"The least space between covers in the Albums and Artists views, in pixels. A row spreads out to fill the "
     L"width, up to twice this.", TweakType::Float, 4, 48, 16, [] { return (double)g.coverGap; },
     [](double v) { g.coverGap = std::clamp((int)std::lround(v), 4, 48); }, 0, LibViewRelayout, nullptr, L"px"},
    {"library.browser_height", L"Library", L"BrowserHeight", L"Column browser height",
     L"The column browser's height in pixels. Drag the splitter under it to change it, too. "
     L"Startup defaults offers to keep it for the next start.",
     TweakType::Float, 60, 2000, 150, [] { return (double)g.browserHeight; },
     [](double v) { g.browserHeight = std::clamp((int)std::lround(v), 60, 2000); }, 0,
     [] {
         if (g.hwnd) Layout();
     },
     nullptr, L"px"},
    {"view.wave_style", L"Waveform", L"Left", L"Waveform style",
     L"How the seek bar draws: plain bars, the fountain's bars, or the Dark Fountain itself. "
     L"Only drawn in the DELTARUNE theme.",
     TweakType::Choice, 0, 2, 2, [] { return (double)WaveStyleIndex(); },
     [](double v) { SetWaveStyle((int)v); }, IDM_WAVE_STYLE_BASE, nullptr, kWaveStyleNames, nullptr, kTweakRadio},
    {"view.wave_moving", L"Waveform", L"Motion", L"Fountain moving",
     L"The fountain keeps moving while a track plays and slows to a stop on pause, like the game.",
     TweakType::Bool, 0, 1, 1, [] { return (double)WaveMoving(); },
     [](double v) { SetWaveMoving(v != 0); }, IDM_WAVE_MOTION},
    {"view.wave_deltarune_colors", L"Waveform", L"DeltaruneColors", L"Fountain colours",
     L"The fountain's light follows DELTARUNE's hue wheel instead of the position's played colour.",
     TweakType::Bool, 0, 1, 1, [] { return (double)WaveDeltaruneColors(); },
     [](double v) { SetWaveDeltaruneColors(v != 0); }, IDM_WAVE_DR_COLORS},
    {"view.wave_glow", L"Waveform", L"Glow", L"Fountain glow",
     L"The room behind the fountain in the game's colors, instead of black.",
     TweakType::Bool, 0, 1, 1, [] { return (double)WaveGlow(); },
     [](double v) { SetWaveGlow(v != 0); }, IDM_WAVE_GLOW},
    {"view.wave_black", L"Waveform", L"Black", L"Black surroundings",
     L"The fountain's backdrop is black, as in the game, instead of the bar's surface color.",
     TweakType::Bool, 0, 1, 1, [] { return (double)WaveBlack(); },
     [](double v) { SetWaveBlack(v != 0); }, IDM_WAVE_BLACK},
    {"view.apollo_analyzer", L"Apollo", L"Analyzer", L"Frequency analyzer",
     L"Apollo's frequency analyzer band under the toolbar.",
     TweakType::Bool, 0, 1, 1, [] { return (double)ApolloAnalyzer(); },
     [](double v) { SetApolloAnalyzer(v != 0); }, 0, [] { ApolloSync(); }, nullptr, nullptr, kTweakLegacyLoad},
    {"view.apollo_scope", L"Apollo", L"Oscilloscope", L"Oscilloscope",
     L"Apollo's oscilloscope band under the toolbar.",
     TweakType::Bool, 0, 1, 1, [] { return (double)ApolloScope(); },
     [](double v) { SetApolloScope(v != 0); }, 0, [] { ApolloSync(); }, nullptr, nullptr, kTweakLegacyLoad},
    {"view.apollo_rebar", L"Apollo", L"Rebar", L"Apollo toolbar background",
     L"Which of Apollo's four bitmaps the toolbar uses, or none.",
     TweakType::Choice, 0, 4, 0, [] { return (double)ApolloSkin(); },
     [](double v) { SetApolloSkin((int)v); }, 0, [] { ApolloSync(); }, kSkinNames, nullptr, kTweakLegacyLoad},
    {"view.apollo_opacity", L"Apollo", L"RebarOpacity", L"Apollo toolbar opacity",
     L"How faint the toolbar's background bitmap is, in percent. Grayed with a none background.",
     TweakType::Choice, 0, 4, 1, [] { return (double)std::max(0, OpacityIndex(ApolloOpacity())); },
     [](double v) { static const int kOpacities[5] = {20, 40, 60, 80, 100}; if (int i = (int)v; i >= 0 && i <= 4) SetApolloOpacity(kOpacities[i]); },
     0, [] { ApolloSync(); }, kOpacityNames, nullptr, kTweakLegacyLoad},
    {"view.foobar_view", L"Foobar", L"AlbumListView", L"Album List view",
     L"How the Album List panel groups the library: by folder, by artist and album, by album, or by genre.",
     TweakType::Choice, 0, 3, 0, [] { return (double)FoobarView(); },
     [](double v) { SetFoobarView((int)v); }, 0, [] { FoobarSync(); }, kFoobarViewNames, nullptr, kTweakLegacyLoad},
    // Window layout: each choice shows or hides one part of the normal window's player, or sets
    // where it docks and how the waveform fills it. They change the live layout (g.layout) and are
    // kept for startup by the review, like the layout menu. The section/key fields name where the
    // same choice lives in the saved layout; SaveTweaks skips them because [Layout] Parts is one
    // string (SaveMainLayout), not one key per part.
    {"window.player_dock", L"Layout", L"Player", L"Player position",
     L"Where the player sits in the normal window: top, bottom, left or right.",
     TweakType::Choice, 0, 3, 0, [] { return (double)(int)g.layout.player; },
     [](double v) { g.layout.player = (Dock)(int)v; }, 0, ApplyMainLayout, kDockNames},
    {"window.wave_size", L"Layout", L"Wave", L"Waveform size",
     L"How tall the waveform band is, or how wide the column: slim, medium or tall.",
     TweakType::Choice, 0, 2, 0, [] { return (double)g.layout.waveSize; },
     [](double v) { g.layout.waveSize = (uint8_t)v; }, 0, ApplyMainLayout, kWaveSizeNames},
    {"window.wave_own_row", L"Layout", L"WaveRow", L"Waveform on its own row",
     L"At the top or bottom, the waveform on a line of its own under the buttons.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.layout.waveOwnRow; },
     [](double v) { g.layout.waveOwnRow = v != 0; }, 0, ApplyMainLayout},
    {"window.panels_left", L"Layout", L"PanelsLeft", L"Side panels on the left",
     L"The side panels (Up next, lyrics) take the left side instead of the right.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.layout.panelsLeft; },
     [](double v) { g.layout.panelsLeft = v != 0; }, 0, ApplyMainLayout},
    {"window.transport", L"Layout", L"PartsTransport", L"Show previous, play and next",
     L"The transport buttons in the player.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.layout.transport; },
     [](double v) { g.layout.transport = v != 0; }, 0, ApplyMainLayout},
    {"window.order", L"Layout", L"PartsOrder", L"Show shuffle and repeat",
     L"The shuffle and repeat buttons in the player.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.layout.order; },
     [](double v) { g.layout.order = v != 0; }, 0, ApplyMainLayout},
    {"window.wave", L"Layout", L"PartsWaveform", L"Show waveform seek bar",
     L"The waveform seek bar in the player.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.layout.wave; },
     [](double v) { g.layout.wave = v != 0; }, 0, ApplyMainLayout},
    {"window.times", L"Layout", L"PartsTimes", L"Show times",
     L"The elapsed and remaining times beside the waveform.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.layout.times; },
     [](double v) { g.layout.times = v != 0; }, 0, ApplyMainLayout},
    {"window.volume", L"Layout", L"PartsVolume", L"Show volume",
     L"The volume control in the player.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.layout.volume; },
     [](double v) { g.layout.volume = v != 0; }, 0, ApplyMainLayout},
    {"window.now_playing", L"Layout", L"PartsNowPlaying", L"Show now playing",
     L"The now playing line: cover, title and artist.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.layout.nowPlaying; },
     [](double v) { g.layout.nowPlaying = v != 0; }, 0, ApplyMainLayout},
    {"window.large_cover", L"Layout", L"PartsLargeCover", L"Show large cover",
     L"A larger cover in the now playing line.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.layout.largeCover; },
     [](double v) { g.layout.largeCover = v != 0; }, 0, ApplyMainLayout},
    {"window.points", L"Layout", L"PartsPoints", L"Show level and points",
     L"The level and points chip beside now playing.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.layout.points; },
     [](double v) { g.layout.points = v != 0; }, 0, ApplyMainLayout},
    {"window.tabs", L"Layout", L"PartsTabs", L"Show playlist tabs",
     L"The playlist tabs above the track list.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.layout.tabs; },
     [](double v) { g.layout.tabs = v != 0; }, 0, ApplyMainLayout},
    {"window.search", L"Layout", L"PartsSearch", L"Show search box",
     L"The search box beside the playlist tabs.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.layout.search; },
     [](double v) { g.layout.search = v != 0; }, 0, ApplyMainLayout},
    {"window.views", L"Layout", L"PartsViews", L"Show library view buttons",
     L"The Library's view buttons above the track list.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.layout.views; },
     [](double v) { g.layout.views = v != 0; }, 0, ApplyMainLayout},
    {"window.up_next", L"Layout", L"PartsUpNext", L"Show up next panel",
     L"The Up next panel beside the track list.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.layout.upNext; },
     [](double v) { g.layout.upNext = v != 0; }, 0, ApplyMainLayout},
    {"window.status", L"Layout", L"PartsStatus", L"Show status bar",
     L"The status bar at the bottom of the window.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.layout.status; },
     [](double v) { g.layout.status = v != 0; }, 0, ApplyMainLayout},
    {"window.menu_bar", L"Layout", L"PartsMenuBar", L"Show menu bar",
     L"The menu bar. When hidden, Alt or a right-click on the player still opens the menus.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.layout.menuBar; },
     [](double v) { g.layout.menuBar = v != 0; }, 0, ApplyMainLayout},
    {"window.tab_counts", L"Layout", L"PartsTabCounts", L"Show track counts on tabs",
     L"The item count after each tab's name.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.layout.tabCounts; },
     [](double v) { g.layout.tabCounts = v != 0; }, 0, ApplyMainLayout},
    {"window.status_details", L"Layout", L"PartsStatusDetails", L"Show audio details in the status bar",
     L"ReplayGain, equalizer and output details in the status bar.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.layout.statusDetails; },
     [](double v) { g.layout.statusDetails = v != 0; }, 0, ApplyMainLayout},
    {"window.hints", L"Layout", L"PartsHints", L"Show playback hints",
     L"How-to hints in the now playing line.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.layout.hints; },
     [](double v) { g.layout.hints = v != 0; }, 0, ApplyMainLayout},
};

// The window's pages: All settings, then one per key prefix, in the groups the page list captions.
// A page may also link to related windows or actions, using the same commands as the menus.
struct Link {
    int command;
    const wchar_t* label;
};
struct Page {
    const char* prefix;    // null: every setting
    const wchar_t* name;
    const wchar_t* about;  // the line under the page's title
    const wchar_t* group;  // the caption above the page in the list, where a group starts
    Link links[4];
};
const Page kPages[] = {
    {nullptr, L"All settings", L"Every setting on one page, under the page it belongs to.", nullptr, {}},
    {"playback.", L"Playback", L"Resuming, how tracks change, shuffle and radio, and this session's speed.", L"Sound", {}},
    {"output.", L"Output", L"Exclusive output and the volume control.", nullptr,
     {{IDM_HEADPHONES, L"&Headphones and devices..."}}},
    {"replaygain.", L"ReplayGain", L"Even loudness from track to track, and measuring it.", nullptr,
     {{IDM_RG_SCAN_SEL, L"Scan &selection"}, {IDM_RG_SCAN_PL, L"Scan &playlist"},
      {IDM_RG_SCAN_ALL, L"Scan &library"}, {IDM_RG_CANCEL, L"&Cancel scan"}}},
    {"effects.", L"Effects", L"Reverb, volume shaping and bitcrush, after the equalizer.", nullptr,
     {{IDM_EQUALIZER, L"&Equalizer..."}}},
    {"view.", L"View", L"The seek bar and its colours, and how the window behaves.", L"Look",
     {{IDM_FORMAT, L"Title &formatting..."}}},
    {"window.", L"Layout", L"Where the player sits and which parts of the normal window show.", nullptr,
     {{IDM_STARTUP_DEFAULTS, L"&Startup defaults..."}}},
    {"library.", L"Library", L"New music, sizes in the library views, and Explorer and the web server.", L"Collection",
     {{IDM_LIBRARY_FOLDERS, L"Library &folders..."}, {IDM_WEB_SERVER, L"&Web server..."}}},
    {"convert.", L"Conversion", L"Converting to .cord, and putting the originals back.", nullptr, {}},
    {"online.", L"Online", L"Covers, lyrics and artist info from the web. Each says what it sends, and all start off.",
     nullptr, {{IDM_FETCH_COVERS, L"&Fetch missing covers..."}}},
    {"scrobbling.", L"Scrobbling", L"Listens sent to connected services, and your Discord status.", nullptr,
     {{IDM_LISTENBRAINZ, L"&Scrobbling..."}}},
    {"startup.", L"Startup", L"How cordinal opens, and what it keeps for next time.", L"General",
     {{IDM_STARTUP_DEFAULTS, L"&Startup defaults..."}}},
};
constexpr int kLinks = (int)std::size(kPages[0].links);

// Headings inside a page, each above the setting it names and the ones after it on that page.
struct Heading {
    const char* key;
    const wchar_t* title;
};
const Heading kHeadings[] = {
    {"playback.resume", L"Resuming"}, {"playback.crossfade", L"Track changes"},
    {"playback.skip_aware_shuffle", L"Shuffle and radio"}, {"playback.speed", L"Speed and time"},
    {"output.exclusive", L"Device"}, {"output.software_volume", L"Volume"},
    {"replaygain.enabled", L"Levelling"}, {"replaygain.auto_measure", L"Measuring"},
    {"effects.reverb", L"Reverb"}, {"effects.loudness_compensation", L"Volume shaping"}, {"effects.bitcrush", L"Bitcrush"},
    {"view.seek_bar", L"Seek bar"}, {"view.wave_color", L"Colours"}, {"view.smooth_seek", L"Shape"},
    {"view.group_albums", L"Window"}, {"view.sound_effects", L"Sounds"}, {"view.wave_style", L"Dark Fountain"},
    {"view.apollo_analyzer", L"Apollo"}, {"view.foobar_view", L"Album List"},
    {"window.player_dock", L"Arrangement"}, {"window.transport", L"Parts shown"},
    {"library.new_music", L"Adding music"}, {"library.explorer_menu", L"Outside cordinal"},
    {"library.cover_size", L"Sizes"},
    {"online.covers_deezer", L"Covers"}, {"online.lyrics_lrclib", L"Lyrics"}, {"online.artist_info", L"Artist info"},
};

bool OnPage(const Tweak& t, int page) {
    const char* prefix = kPages[page].prefix;
    return !prefix || !strncmp(t.key, prefix, strlen(prefix));
}

int PageOf(const Tweak& t) {
    for (int i = 1; i < (int)std::size(kPages); ++i)
        if (OnPage(t, i)) return i;
    return 0;
}

std::wstring KeyText(const Tweak& t) { return std::wstring(t.key, t.key + strlen(t.key)); }

// The heading each setting sits under on its own page: the last one at or before it in the
// registry, so a heading still shows when the setting it names is filtered out.
const wchar_t* HeadingOf(int tweak) {
    const int page = PageOf(kTweaks[tweak]);
    const wchar_t* heading = nullptr;
    for (int i = 0; i <= tweak; ++i) {
        if (PageOf(kTweaks[i]) != page) continue;
        for (const Heading& h : kHeadings)
            if (!strcmp(h.key, kTweaks[i].key)) heading = h.title;
    }
    return heading;
}

constexpr int kIdSearch = 100, kIdFilter = 101, kIdSheet = 102, kIdResetAll = 106, kIdPages = 107, kIdLink0 = 110,
              kIdRow0 = 1000, kIdReset0 = 3000;
static_assert(kIdRow0 + (int)std::size(kTweaks) <= kIdReset0);

// One line of the sheet: a heading, or a setting with its label, help, control and reset.
struct SheetRow {
    int tweak = -1;                    // kTweaks index, or -1 for a heading
    const wchar_t* heading = nullptr;
    bool pageHeading = false;          // All settings: a page's name, above its own headings
    int y = 0, h = 0;                  // in the sheet's content, before scrolling
    int labelH = 0, helpH = 0;
    HWND label = nullptr, help = nullptr, control = nullptr, unit = nullptr, reset = nullptr;
};
enum StaticKind { kLabel, kHelp, kSubHeading, kPageHeading };

struct PreferencesWindow {
    HWND hwnd = nullptr, pages = nullptr, search = nullptr, filter = nullptr, sheet = nullptr, title = nullptr;
    HWND about = nullptr, resetAll = nullptr, close = nullptr;
    HWND links[kLinks] = {};
    HFONT titleFont = nullptr;
    HBRUSH currentBrush = nullptr;  // the focused setting's row
    std::vector<SheetRow> rows;
    int page = 0;           // kPages index
    int current = -1;       // the kTweaks index focused or asked for
    int scroll = 0, contentH = 0, wheel = 0;
    bool filling = false;   // the controls are being filled in, so their notifications are not changes
} w;

bool Searching() { return GetWindowTextLengthW(w.search) > 0; }
bool ChangedOnly() { return SendMessageW(w.filter, CB_GETCURSEL, 0, 0) == 1; }

void LayoutPreferences();

bool LinkShown(const Link& link) {
    return link.label && !Searching() && (link.command != IDM_RG_CANCEL || (g.rgRunning && !g.rgAuto));
}

bool LinkEnabled(int command) {
    const bool manual = g.rgRunning && !g.rgAuto;
    switch (command) {
        case IDM_RG_SCAN_SEL: return !manual && !SelectedTracks().empty();
        case IDM_RG_SCAN_PL: return !manual && !Active().items.empty();
        case IDM_RG_SCAN_ALL: return !manual && replaygain::HasLibraryTracks();
        case IDM_RG_CANCEL: return manual;
        default: return true;
    }
}

void RefreshLinks() {
    bool layout = false;
    for (int i = 0; i < kLinks; ++i) {
        const Link& link = kPages[w.page].links[i];
        const bool visible = LinkShown(link);
        layout |= ((GetWindowLongPtrW(w.links[i], GWL_STYLE) & WS_VISIBLE) != 0) != visible;
        if (link.label) SetTextIfChanged(w.links[i], link.label);
        ShowWindow(w.links[i], visible ? SW_SHOWNA : SW_HIDE);
        EnableWindow(w.links[i], LinkEnabled(link.command));
    }
    if (layout) LayoutPreferences();
}

// A typed value as it reads best: 16, 0.4, -2.5.
std::wstring NumberText(double value) {
    wchar_t text[32];
    swprintf_s(text, L"%.2f", value);
    std::wstring s = text;
    while (s.size() > 1 && s.back() == L'0') s.pop_back();
    if (!s.empty() && s.back() == L'.') s.pop_back();
    return s == L"-0" ? L"0" : s;
}

int RowOf(int tweak) {
    for (size_t i = 0; i < w.rows.size(); ++i)
        if (w.rows[i].tweak == tweak && tweak >= 0) return (int)i;
    return -1;
}

int SheetHeight() {
    RECT rc;
    GetClientRect(w.sheet, &rc);
    return rc.bottom;
}

// Heights and places for every row at the sheet's width, then the scroll range.
void LayoutSheet() {
    if (!w.sheet) return;
    RECT rc;
    GetClientRect(w.sheet, &rc);
    const int width = rc.right, pad = S(12), gap = S(20);
    const int controlW = std::clamp(width * 2 / 5, S(150), S(280)), cx = width - pad - controlW;
    const int textW = std::max(S(120), cx - gap - pad);
    HDC dc = GetDC(w.sheet);
    int y = 0;
    for (SheetRow& row : w.rows) {
        row.y = y;
        if (row.tweak < 0) {
            row.h = row.pageHeading ? S(52) : S(40);
        } else {
            auto measure = [&](HFONT font, HWND text) {
                RECT r{0, 0, textW, 0};
                HGDIOBJ old = SelectObject(dc, font);
                std::wstring s = WindowText(text);
                DrawTextW(dc, s.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                SelectObject(dc, old);
                return (int)r.bottom;
            };
            row.labelH = measure(g.fontBold, row.label);
            row.helpH = measure(g.font, row.help);
            const int text = S(12) + row.labelH + S(3) + row.helpH + S(14);
            const int controls = S(12) + S(26) + S(6) + S(24) + S(12);
            row.h = std::max(text, controls);
        }
        y += row.h;
    }
    ReleaseDC(w.sheet, dc);
    w.contentH = y;
    w.scroll = std::clamp(w.scroll, 0, std::max(0, w.contentH - (int)rc.bottom));
    HDWP dwp = BeginDeferWindowPos((int)w.rows.size() * 4);
    auto place = [&](HWND h, int x, int top, int cw, int ch) {
        if (h && dwp) dwp = DeferWindowPos(dwp, h, nullptr, x, top, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    for (const SheetRow& row : w.rows) {
        const int top = row.y - w.scroll;
        if (row.tweak < 0) {
            place(row.label, pad, top + row.h - S(30), width - pad * 2, S(22));
            continue;
        }
        const Tweak& t = kTweaks[row.tweak];
        place(row.label, pad, top + S(12), textW, row.labelH);
        place(row.help, pad, top + S(12) + row.labelH + S(3), textW, row.helpH);
        if (t.type == TweakType::Float) {
            place(row.control, cx, top + S(12), S(90), S(26));
            place(row.unit, cx + S(98), top + S(15), S(60), S(20));
        } else {
            place(row.control, cx, top + S(12), t.type == TweakType::Bool ? S(120) : controlW,
                  t.type == TweakType::Bool ? S(26) : S(300));
        }
        place(row.reset, cx, top + S(44), S(84), S(24));
    }
    if (dwp) EndDeferWindowPos(dwp);
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, w.contentH - 1), (UINT)rc.bottom, w.scroll};
    SetScrollInfo(w.sheet, SB_VERT, &si, TRUE);
    InvalidateRect(w.sheet, nullptr, FALSE);
}

void ScrollSheetTo(int y) {
    const int top = std::clamp(y, 0, std::max(0, w.contentH - SheetHeight()));
    if (top == w.scroll) return;
    const int dy = w.scroll - top;
    w.scroll = top;
    ScrollWindowEx(w.sheet, 0, dy, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE);
    SetScrollPos(w.sheet, SB_VERT, w.scroll, TRUE);
}

// Brings a row wholly into view, with the heading above it when that fits too.
void RevealRow(int index) {
    if (index < 0 || index >= (int)w.rows.size()) return;
    const SheetRow& row = w.rows[index];
    int top = row.y;
    if (index > 0 && w.rows[index - 1].tweak < 0) top = w.rows[index - 1].y;
    const int height = SheetHeight();
    if (top < w.scroll) ScrollSheetTo(top);
    else if (row.y + row.h > w.scroll + height) ScrollSheetTo(std::min(top, row.y + row.h - height));
}

void RedrawRow(int tweak) {
    int index = RowOf(tweak);
    if (index < 0) return;
    RECT rc;
    GetClientRect(w.sheet, &rc);
    RECT r{0, w.rows[index].y - w.scroll, rc.right, w.rows[index].y - w.scroll + w.rows[index].h};
    RedrawWindow(w.sheet, &r, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void SetCurrent(int tweak) {
    if (tweak == w.current) return;
    const int old = w.current;
    w.current = tweak;
    RedrawRow(old);
    RedrawRow(tweak);
    RevealRow(RowOf(tweak));
}

// The controls follow the values, which may have changed from the menus or the palette.
void RefreshValues() {
    if (!w.hwnd) return;
    w.filling = true;
    for (const SheetRow& row : w.rows) {
        if (row.tweak < 0) continue;
        const Tweak& t = kTweaks[row.tweak];
        if (t.type == TweakType::Bool) InvalidateRect(row.control, nullptr, FALSE);
        else if (t.type == TweakType::Choice) {
            const int value = std::clamp((int)t.get(), 0, (int)t.max);
            if (SendMessageW(row.control, CB_GETCURSEL, 0, 0) != value) SendMessageW(row.control, CB_SETCURSEL, value, 0);
        } else if (GetFocus() != row.control) SetTextIfChanged(row.control, NumberText(t.get()));
        const bool changed = TweakChanged(t);
        if (((GetWindowLongPtrW(row.reset, GWL_STYLE) & WS_VISIBLE) != 0) != changed) {
            if (!changed && GetFocus() == row.reset) SetFocus(row.control);
            ShowWindow(row.reset, changed ? SW_SHOWNA : SW_HIDE);
        }
    }
    w.filling = false;
    // The bottom reset covers what the sheet shows: everything, this page, or the search results.
    bool any = false;
    for (const SheetRow& row : w.rows) any = any || (row.tweak >= 0 && TweakChanged(kTweaks[row.tweak]));
    SetTextIfChanged(w.resetAll, Searching() ? L"&Reset these to defaults" : w.page ? L"&Reset page to defaults"
                                                                                  : L"&Reset all to defaults");
    EnableWindow(w.resetAll, any);
    InvalidateRect(w.pages, nullptr, FALSE);  // the changed counts
    RefreshLinks();
}

LRESULT CALLBACK WheelToSheet(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    // A closed choice list never takes the wheel: scrolling the page must not change settings on the way.
    if (msg == WM_MOUSEWHEEL && !SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0)) return SendMessageW(w.sheet, msg, wp, lp);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, WheelToSheet, 0);
    return DefSubclassProc(h, msg, wp, lp);
}

// Every row's windows, made afresh for what the sheet shows, in tab order.
void BuildSheet() {
    for (const SheetRow& row : w.rows)
        for (HWND h : {row.label, row.help, row.control, row.unit, row.reset})
            if (h) DestroyWindow(h);
    auto make = [&](DWORD ex, const wchar_t* cls, const std::wstring& text, DWORD style, int id, HFONT font) {
        HWND c = CreateWindowExW(ex, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, w.sheet,
                                 (HMENU)(INT_PTR)id, g.inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
        return c;
    };
    for (size_t i = 0; i < w.rows.size(); ++i) {
        SheetRow& row = w.rows[i];
        auto tag = [&](HWND h, StaticKind kind) { SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)(i * 4 + kind)); };
        if (row.tweak < 0) {
            row.label = make(0, WC_STATICW, row.heading, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0,
                             row.pageHeading ? w.titleFont : g.fontBold);
            tag(row.label, row.pageHeading ? kPageHeading : kSubHeading);
            continue;
        }
        const Tweak& t = kTweaks[row.tweak];
        row.label = make(0, WC_STATICW, t.label, SS_LEFT | SS_NOPREFIX, 0, g.fontBold);
        tag(row.label, kLabel);
        std::wstring help = std::wstring(t.help) + L" Default: " +
                            (t.type == TweakType::Float ? NumberText(t.def) + (t.unit ? std::wstring(L" ") + t.unit : L"")
                                                        : TweakText(t, t.def)) + L".";
        if (Searching()) help += std::wstring(L" (") + kPages[PageOf(t)].name + L")";
        row.help = make(0, WC_STATICW, help, SS_LEFT | SS_NOPREFIX, 0, g.font);
        tag(row.help, kHelp);
        const int id = kIdRow0 + row.tweak;
        if (t.type == TweakType::Bool) {
            // An owner-drawn switch named for its setting, so a screen reader says what it turns on.
            row.control = make(0, WC_BUTTONW, t.label, BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP, id, g.font);
        } else if (t.type == TweakType::Choice) {
            row.control = make(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, g.font);
            for (int c = 0; c <= (int)t.max; ++c)
                SendMessageW(row.control, CB_ADDSTRING, 0, (LPARAM)(t.choices ? t.choices[c] : kOffOn[c]));
            SendMessageW(row.control, CB_SETCURSEL, (WPARAM)std::clamp((int)t.get(), 0, (int)t.max), 0);
            SetWindowTheme(row.control, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            SetWindowSubclass(row.control, WheelToSheet, 0, 0);
        } else {
            row.control = make(WS_EX_CLIENTEDGE, WC_EDITW, NumberText(t.get()), ES_AUTOHSCROLL | WS_TABSTOP, id, g.font);
            SetWindowTheme(row.control, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            row.unit = make(0, WC_STATICW, t.unit ? t.unit : L"", SS_LEFT | SS_NOPREFIX, 0, g.font);
            tag(row.unit, kHelp);
        }
        row.reset = make(0, WC_BUTTONW, L"Reset", BS_PUSHBUTTON | BS_NOTIFY | WS_TABSTOP, kIdReset0 + row.tweak, g.font);
        ApplyControlTheme(row.reset, g.dark);
        ShowWindow(row.reset, TweakChanged(t) ? SW_SHOWNA : SW_HIDE);
    }
}

void Rebuild(int keep) {
    std::wstring query = WindowText(w.search);
    const bool changedOnly = ChangedOnly();
    // A search covers every page, so the page list says so.
    if (!query.empty() && w.page != 0) {
        w.page = 0;
        SendMessageW(w.pages, LB_SETCURSEL, 0, 0);
    }
    auto wanted = [&](int i) { return TweakShown(kTweaks[i]) && (!changedOnly || TweakChanged(kTweaks[i])); };
    std::vector<int> picks;
    if (!query.empty()) {
        std::vector<std::wstring> texts;
        for (const Tweak& t : kTweaks)
            texts.push_back(std::wstring(t.label) + L" " + kPages[PageOf(t)].name + L" " + KeyText(t) + L" " + t.help);
        for (int i : RankMatches(texts, query))
            if (wanted(i)) picks.push_back(i);
    } else {
        for (int i = 0; i < (int)std::size(kTweaks); ++i)
            if (wanted(i) && OnPage(kTweaks[i], w.page)) picks.push_back(i);
        // All settings reads as every page in turn, each under its name.
        std::stable_sort(picks.begin(), picks.end(), [](int a, int b) { return PageOf(kTweaks[a]) < PageOf(kTweaks[b]); });
    }
    std::vector<SheetRow> rows;
    int lastPage = -1;
    const wchar_t* lastHeading = nullptr;
    for (int i : picks) {
        if (query.empty()) {
            const int page = PageOf(kTweaks[i]);
            if (w.page == 0 && page != lastPage) {
                rows.push_back({-1, kPages[page].name, true});
                lastHeading = nullptr;
            }
            lastPage = page;
            const wchar_t* heading = HeadingOf(i);
            if (heading && heading != lastHeading) rows.push_back({-1, heading});
            lastHeading = heading;
        }
        SheetRow row;
        row.tweak = i;
        rows.push_back(row);
    }
    SetWindowRedraw(w.sheet, FALSE);
    for (const SheetRow& row : w.rows)
        for (HWND h : {row.label, row.help, row.control, row.unit, row.reset})
            if (h) DestroyWindow(h);
    w.rows.clear();
    w.rows = std::move(rows);
    w.scroll = 0;
    BuildSheet();
    w.current = RowOf(keep) >= 0 ? keep : -1;
    SetTextIfChanged(w.title, !query.empty() ? std::wstring(L"Search") : std::wstring(kPages[w.page].name));
    const int found = (int)picks.size();
    SetTextIfChanged(w.about, !query.empty() ? (found == 1 ? std::wstring(L"1 setting matches, from every page.")
                                                           : std::to_wstring(found) + L" settings match, from every page.")
                                             : std::wstring(kPages[w.page].about));
    // Redraw comes back before the layout: the sheet moves when a page has links, and a window
    // moved with redraw off leaves its old place unpainted, showing the last page's text.
    SetWindowRedraw(w.sheet, TRUE);
    LayoutPreferences();
    RevealRow(RowOf(w.current));
    RedrawWindow(w.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    RefreshValues();
}

void CommitNumber(int tweak) {
    int index = RowOf(tweak);
    if (index < 0 || kTweaks[tweak].type != TweakType::Float) return;
    HWND edit = w.rows[index].control;
    std::wstring text = WindowText(edit);
    wchar_t* end = nullptr;
    double v = wcstod(text.c_str(), &end);
    if (end != text.c_str()) SetTweak(kTweaks[tweak], v);
    SetWindowTextW(edit, NumberText(kTweaks[tweak].get()).c_str());  // a value out of range shows where it was clamped to
    RefreshValues();
}

void LayoutPreferences() {
    if (!w.hwnd) return;
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int pad = S(16), navW = S(188), buttonH = S(28), bottom = rc.bottom - pad - buttonH;
    MoveWindow(w.pages, pad, pad, navW, std::max((int)rc.bottom - pad * 2, S(80)), TRUE);
    const int x = pad + navW + S(22), width = rc.right - x - pad, filterW = S(170);
    int y = pad;
    MoveWindow(w.search, x, y, width - filterW - S(8), S(26), TRUE);
    MoveWindow(w.filter, x + width - filterW, y, filterW, S(200), TRUE);
    y += S(42);
    MoveWindow(w.title, x, y, width, S(30), TRUE);
    y += S(32);
    MoveWindow(w.about, x, y, width, S(20), TRUE);
    y += S(30);
    int linkCount = 0;
    for (const Link& link : kPages[w.page].links) linkCount += LinkShown(link);
    const int linkW = std::min(S(190), (width - S(8) * std::max(0, linkCount - 1)) / std::max(1, linkCount));
    for (int i = 0, lx = x; i < kLinks; ++i) {
        if (!LinkShown(kPages[w.page].links[i])) continue;
        MoveWindow(w.links[i], lx, y, linkW, buttonH, TRUE);
        lx += linkW + S(8);
    }
    if (linkCount) y += buttonH + S(10);
    // The sheet reaches a row's padding left of the title, so labels line up under it.
    MoveWindow(w.sheet, x - S(12), y, width + S(12), std::max(bottom - S(12) - y, S(80)), TRUE);
    MoveWindow(w.resetAll, x, bottom, S(190), buttonH, TRUE);
    MoveWindow(w.close, rc.right - pad - S(90), bottom, S(90), buttonH, TRUE);
    LayoutSheet();
}

void DrawPage(const DRAWITEMSTRUCT* di) {
    if (di->itemID == (UINT)-1) return;
    const Page& page = kPages[di->itemID];
    const bool selected = di->itemState & ODS_SELECTED;
    RECT r = di->rcItem;
    SetBkMode(di->hDC, TRANSPARENT);
    if (page.group) {  // the group's caption, above its first page
        RECT caption = r;
        caption.bottom = r.top + S(30);
        FillSolid(di->hDC, caption, Ref(P().surface));
        caption.left += S(12);
        caption.bottom -= S(5);
        HGDIOBJ old = SelectObject(di->hDC, g.fontBold);
        SetTextColor(di->hDC, Ref(P().sub));
        DrawTextW(di->hDC, page.group, -1, &caption, DT_SINGLELINE | DT_BOTTOM | DT_NOPREFIX);
        SelectObject(di->hDC, old);
        r.top += S(30);
    }
    FillSolid(di->hDC, r, Ref(selected ? P().select : P().surface));
    if (selected) FillSolid(di->hDC, {r.left, r.top + S(7), r.left + S(3), r.bottom - S(7)}, Ref(P().accent));
    HGDIOBJ old = SelectObject(di->hDC, selected ? g.fontBold : g.font);
    r.left += S(14);
    r.right -= S(10);
    int changed = 0;
    for (const Tweak& t : kTweaks) changed += TweakShown(t) && OnPage(t, (int)di->itemID) && TweakChanged(t);
    if (changed) {  // how many settings here differ from their defaults
        SetTextColor(di->hDC, Ref(selected ? P().selectText : P().sub));
        std::wstring count = std::to_wstring(changed);
        DrawTextW(di->hDC, count.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }
    SetTextColor(di->hDC, Ref(selected ? P().selectText : P().text));
    DrawTextW(di->hDC, page.name, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(di->hDC, old);
}

// An on/off setting as a switch: the track lit in the accent when on, with the knob at that end.
void DrawSwitch(const DRAWITEMSTRUCT* di) {
    const int tweak = (int)di->CtlID - kIdRow0;
    if (tweak < 0 || tweak >= (int)std::size(kTweaks)) return;
    const bool on = kTweaks[tweak].get() != 0, enabled = !(di->itemState & ODS_DISABLED);
    RECT r = di->rcItem;
    FillSolid(di->hDC, r, Ref(tweak == w.current ? P().hover : P().surface));
    const int trackH = S(20), trackW = S(38), top = r.top + (r.bottom - r.top - trackH) / 2;
    RECT track{r.left + S(2), top, r.left + S(2) + trackW, top + trackH};
    const Rgb fill = on && enabled ? P().accent : P().border;
    HBRUSH brush = CreateSolidBrush(Ref(fill));
    HPEN pen = CreatePen(PS_SOLID, 1, Ref(fill));
    HGDIOBJ oldBrush = SelectObject(di->hDC, brush), oldPen = SelectObject(di->hDC, pen);
    RoundRect(di->hDC, track.left, track.top, track.right, track.bottom, trackH, trackH);
    const int knob = trackH - S(6), kx = on ? track.right - S(3) - knob : track.left + S(3);
    HBRUSH knobBrush = CreateSolidBrush(Ref(on && enabled ? TextOn(P().accent) : P().text));
    HPEN knobPen = CreatePen(PS_SOLID, 1, Ref(on && enabled ? TextOn(P().accent) : P().text));
    SelectObject(di->hDC, knobBrush);
    SelectObject(di->hDC, knobPen);
    Ellipse(di->hDC, kx, track.top + S(3), kx + knob, track.top + S(3) + knob);
    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT)) {
        HPEN ring = CreatePen(PS_SOLID, std::max(1, S(2)), Ref(P().accent));
        SelectObject(di->hDC, ring);
        SelectObject(di->hDC, GetStockObject(HOLLOW_BRUSH));
        RoundRect(di->hDC, track.left - S(2), track.top - S(2), track.right + S(2), track.bottom + S(2), trackH + S(4),
                  trackH + S(4));
        SelectObject(di->hDC, knobPen);
        DeleteObject(ring);
    }
    SelectObject(di->hDC, oldBrush);
    SelectObject(di->hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    DeleteObject(knobBrush);
    DeleteObject(knobPen);
    RECT text{track.right + S(10), r.top, r.right, r.bottom};
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, Ref(enabled ? P().text : P().faint));
    HGDIOBJ oldFont = SelectObject(di->hDC, g.font);
    DrawTextW(di->hDC, on ? L"On" : L"Off", -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(di->hDC, oldFont);
}

// The sheet's own paint: row backgrounds, the focused row's accent, the rules between rows and a
// note when nothing is shown. Labels, help and headings are static controls on top.
void PaintSheet(HDC dc, const RECT& rc) {
    FillSolid(dc, rc, Ref(P().surface));
    const int pad = S(12);
    for (size_t i = 0; i < w.rows.size(); ++i) {
        const SheetRow& row = w.rows[i];
        const int top = row.y - w.scroll;
        if (top >= rc.bottom || top + row.h <= 0) continue;
        if (row.tweak < 0) {  // a rule under the heading's words
            FillSolid(dc, {pad, top + row.h - S(6), rc.right - pad, top + row.h - S(6) + std::max(1, S(1))},
                      Ref(P().border));
            continue;
        }
        if (row.tweak == w.current) {
            FillSolid(dc, {0, top, rc.right, top + row.h}, Ref(P().hover));
            FillSolid(dc, {0, top + S(8), S(3), top + row.h - S(8)}, Ref(P().accent));
        }
        if (i + 1 < w.rows.size() && w.rows[i + 1].tweak >= 0)
            FillSolid(dc, {pad, top + row.h - 1, rc.right - pad, top + row.h}, Ref(P().border));
    }
    if (w.rows.empty()) {
        RECT r{pad, S(12), rc.right - pad, S(60)};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, Ref(P().sub));
        HGDIOBJ old = SelectObject(dc, g.font);
        DrawTextW(dc, Searching() ? L"No setting matches the search." : ChangedOnly() ? L"Nothing has been changed here."
                                                                                        : L"These settings have windows of their own.",
                  -1, &r, DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
    }
}

LRESULT CALLBACK SheetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ old = SelectObject(mem, bmp);
            PaintSheet(mem, rc);
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE:
            if (w.hwnd) LayoutSheet();
            return 0;
        case WM_MOUSEWHEEL: {
            w.wheel += GET_WHEEL_DELTA_WPARAM(wp);
            const int step = w.wheel * S(60) / WHEEL_DELTA;
            if (step) {
                w.wheel -= step * WHEEL_DELTA / S(60);
                ScrollSheetTo(w.scroll - step);
            }
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof(si), SIF_ALL};
            GetScrollInfo(h, SB_VERT, &si);
            int y = w.scroll;
            switch (LOWORD(wp)) {
                case SB_LINEUP: y -= S(24); break;
                case SB_LINEDOWN: y += S(24); break;
                case SB_PAGEUP: y -= (int)si.nPage; break;
                case SB_PAGEDOWN: y += (int)si.nPage; break;
                case SB_TOP: y = 0; break;
                case SB_BOTTOM: y = w.contentH; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: y = si.nTrackPos; break;
            }
            ScrollSheetTo(y);
            return 0;
        }
        case WM_LBUTTONDOWN: {  // a click on a row's words focuses its control
            const int y = GET_Y_LPARAM(lp) + w.scroll;
            for (const SheetRow& row : w.rows)
                if (row.tweak >= 0 && y >= row.y && y < row.y + row.h) SetFocus(row.control);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            const LONG_PTR tag = GetWindowLongPtrW((HWND)lp, GWLP_USERDATA);
            const size_t index = (size_t)tag / 4;
            const int kind = (int)(tag % 4);
            const bool current = index < w.rows.size() && w.rows[index].tweak >= 0 && w.rows[index].tweak == w.current;
            SetTextColor(dc, Ref(kind == kHelp || kind == kSubHeading ? P().sub : P().text));
            SetBkColor(dc, Ref(current ? P().hover : P().surface));
            if (!current) return (LRESULT)g.surfaceBrush;
            if (!w.currentBrush) w.currentBrush = CreateSolidBrush(Ref(P().hover));
            return (LRESULT)w.currentBrush;
        }
        case WM_CTLCOLORBTN:  // the Reset buttons' corners
            return (LRESULT)g.surfaceBrush;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case WM_DRAWITEM:
            DrawSwitch(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id >= kIdReset0 && id < kIdReset0 + (int)std::size(kTweaks)) {
                const int tweak = id - kIdReset0;
                if (code == BN_SETFOCUS) SetCurrent(tweak);
                if (code == BN_CLICKED) {
                    if (int index = RowOf(tweak); index >= 0) SetFocus(w.rows[index].control);
                    SetTweak(kTweaks[tweak], kTweaks[tweak].def);
                    RefreshValues();
                }
                return 0;
            }
            if (id < kIdRow0 || id >= kIdRow0 + (int)std::size(kTweaks)) return 0;
            const int tweak = id - kIdRow0;
            const Tweak& t = kTweaks[tweak];
            if (code == BN_SETFOCUS || code == CBN_SETFOCUS || code == EN_SETFOCUS) {
                SetCurrent(tweak);
            } else if (t.type == TweakType::Bool && (code == BN_CLICKED || code == BN_DOUBLECLICKED)) {
                SetTweak(t, t.get() == 0);
                RefreshValues();
            } else if (t.type == TweakType::Choice && code == CBN_SELCHANGE && !w.filling) {
                SetTweak(t, (double)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0));
                RefreshValues();
            } else if (t.type == TweakType::Float && code == EN_KILLFOCUS && !w.filling) {
                CommitNumber(tweak);
            }
            return 0;
        }
        case WM_NCDESTROY:
            if (w.currentBrush) DeleteObject(w.currentBrush);
            w.currentBrush = nullptr;
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK PreferencesProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id,
                                         g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            LOGFONTW lf{};
            GetObjectW(g.fontBold, sizeof(lf), &lf);
            lf.lfHeight = lf.lfHeight * 4 / 3;
            w.titleFont = CreateFontIndirectW(&lf);
            w.pages = make(0, WC_LISTBOXW, L"Pages", LBS_NOTIFY | LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS |
                           LBS_NOINTEGRALHEIGHT | WS_TABSTOP, kIdPages);
            for (const Page& page : kPages) SendMessageW(w.pages, LB_ADDSTRING, 0, (LPARAM)page.name);
            SendMessageW(w.pages, LB_SETCURSEL, 0, 0);
            w.search = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, kIdSearch);
            SendMessageW(w.search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search every page");
            w.filter = make(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdFilter);
            SendMessageW(w.filter, CB_ADDSTRING, 0, (LPARAM)L"All settings");
            SendMessageW(w.filter, CB_ADDSTRING, 0, (LPARAM)L"Changed from default");
            SendMessageW(w.filter, CB_SETCURSEL, 0, 0);
            w.title = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0);
            SendMessageW(w.title, WM_SETFONT, (WPARAM)w.titleFont, TRUE);
            w.about = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0);
            for (int i = 0; i < kLinks; ++i)
                w.links[i] = make(0, WC_BUTTONW, L"", BS_PUSHBUTTON | WS_TABSTOP, kIdLink0 + i);
            RegisterAppClass(L"CordinalPrefSheet", SheetProc, 0, false);
            w.sheet = make(WS_EX_CONTROLPARENT, L"CordinalPrefSheet", L"Settings", WS_VSCROLL | WS_CLIPCHILDREN,
                           kIdSheet);
            ApplyControlTheme(w.sheet, g.dark);  // its scroll bar
            w.resetAll = make(0, WC_BUTTONW, L"&Reset all to defaults", BS_PUSHBUTTON | WS_TABSTOP, kIdResetAll);
            w.close = make(0, WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            for (HWND c : {w.search, w.filter}) SetWindowTheme(c, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            for (HWND c : {w.resetAll, w.close, w.pages}) ApplyControlTheme(c, g.dark);
            for (HWND c : w.links) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            LayoutPreferences();
            SetTimer(h, 1, 250, nullptr);  // scan state can change while this modeless window stays open
            return 0;
        }
        case WM_TIMER:
            RefreshLinks();
            return 0;
        case WM_SIZE:
            LayoutPreferences();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(700), S(520)};
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) RefreshValues();
            break;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            // The rule between the page list and the page.
            const int x = S(16) + S(188) + S(10);
            FillSolid((HDC)wp, {x, S(16), x + std::max(1, S(1)), rc.bottom - S(16)}, Ref(P().border));
            return 1;
        }
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            mi->itemHeight = S(34) + (mi->itemID < std::size(kPages) && kPages[mi->itemID].group ? S(30) : 0);
            return TRUE;
        }
        case WM_DRAWITEM:
            DrawPage(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_CTLCOLORSTATIC:
            return SurfaceControlColor((HDC)wp, (HWND)lp == w.about ? SubColor() : TextColor());
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if ((HWND)lp == w.pages) return SurfaceControlColor((HDC)wp, TextColor());
            return FieldControlColor((HDC)wp);
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == kIdPages && code == LBN_SELCHANGE) {
                int page = (int)SendMessageW(w.pages, LB_GETCURSEL, 0, 0);
                if (page >= 0 && page != w.page) {
                    w.page = page;
                    if (Searching()) SetWindowTextW(w.search, L"");  // choosing a page ends the search
                    Rebuild(w.current);
                }
            } else if (id == kIdSearch && code == EN_CHANGE) {
                Rebuild(w.current);
            } else if (id == kIdFilter && code == CBN_SELCHANGE) {
                Rebuild(w.current);
            } else if (id >= kIdLink0 && id < kIdLink0 + kLinks) {
                const Link& link = kPages[w.page].links[id - kIdLink0];
                if (int command = link.command; command && LinkShown(link) && LinkEnabled(command))
                    PostMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(command, 0), 0);
            } else if (id == kIdResetAll) {
                int count = 0;
                std::vector<int> shown;
                for (const SheetRow& row : w.rows)
                    if (row.tweak >= 0) shown.push_back(row.tweak);
                for (int i : shown)  // SetTweak refreshes the window as it goes
                    if (TweakChanged(kTweaks[i])) {
                        SetTweak(kTweaks[i], kTweaks[i].def);
                        ++count;
                    }
                ShowMessage(count == 1 ? L"Reset 1 setting to its default." :
                            L"Reset " + std::to_wstring(count) + L" settings to their defaults.");
                Rebuild(w.current);
            } else if (id == IDOK) {  // Enter: commit a typed value
                if (w.current >= 0 && RowOf(w.current) >= 0 && GetFocus() == w.rows[RowOf(w.current)].control)
                    CommitNumber(w.current);
            } else if (id == IDCANCEL) {
                DestroyWindow(h);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_NCDESTROY:
            if (w.titleFont) DeleteObject(w.titleFont);
            w = PreferencesWindow{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}


}  // namespace

std::span<const Tweak> Tweaks() { return kTweaks; }

bool TweakShown(const Tweak& t) {
    if (!discord::Available() && !strcmp(t.key, "scrobbling.discord")) return false;  // no application id yet
    // The Reverb amount slider already covers the menu's presets; a second row for them only repeats it.
    if (!strcmp(t.key, "effects.reverb_mix_choice")) return false;
    if (!kExtraThemes && (!strcmp(t.key, "view.live_fountain") || !strcmp(t.key, "view.robotic_sounds"))) return false;
    // The fountain, Apollo and foobar rows only draw in themes this build leaves out.
    const char* themeOnly[] = {"view.wave_style", "view.wave_moving", "view.wave_deltarune_colors",
                               "view.wave_glow", "view.wave_black", "view.foobar_view"};
    if (!kExtraThemes)
        for (const char* key : themeOnly)
            if (!strcmp(t.key, key)) return false;
    return true;
}

const Tweak* FindTweak(std::string_view key) {
    for (const Tweak& t : kTweaks)
        if (key == t.key) return &t;
    return nullptr;
}

const wchar_t* TweakPage(const Tweak& t) { return kPages[PageOf(t)].name; }

// Floats are saved with two decimals, so anything closer than that is the default.
bool TweakChanged(const Tweak& t) { return std::fabs(t.get() - t.def) > 0.004; }

std::wstring TweakText(const Tweak& t, double value) {
    if (t.type == TweakType::Float) {
        wchar_t text[48];
        swprintf_s(text, t.unit ? L"%.2f %ls" : L"%.2f", value, t.unit);
        return text;
    }
    int i = std::clamp((int)value, 0, (int)t.max);
    return t.choices ? t.choices[i] : kOffOn[i];
}

void SetTweak(const Tweak& t, double value) {
    if (!std::isfinite(value)) return;
    value = std::clamp(value, (double)t.min, (double)t.max);
    if (t.type != TweakType::Float) value = std::round(value);
    if (value == t.get()) return;
    if (t.flags & kTweakRadio) OnCommand(t.command + (int)value, 0, nullptr);
    else if (t.command) OnCommand(t.command, 0, nullptr);
    else {
        t.put(value);
        if (t.apply) t.apply();
        SavePreferences();
    }
    if (w.hwnd) RefreshValues();
}

// Reads each tweak as LoadPreferences always has: an integer is clamped into range, anything not a
// finite number keeps the default.
//
// Rows whose value another record owns are skipped: the window.* rows are the [Layout] Parts string
// (LoadMainLayout), the library size rows are the approved defaults in [Library], and the Apollo
// and foobar2000 rows read and write their feature's own keys. Loading them as plain single keys
// would split one record in two. Seek bar, Seek bar shows and Waveform reflection are two keys each
// (SeekBarKeys).
static bool TweakHasOwnIniRow(const Tweak& t) {
    const char* owned[] = {"library.cover_size", "library.browser_height", "view.apollo_analyzer",
                           "view.apollo_scope", "view.apollo_rebar", "view.apollo_opacity", "view.foobar_view",
                           "view.seek_bar", "view.seek_shows", "view.wave_reflection"};
    if (!strncmp(t.key, "window.", 7)) return false;
    for (const char* key : owned)
        if (!strcmp(t.key, key)) return false;
    return true;
}
// The [Window] keys behind Seek bar, Seek bar shows and Waveform reflection, read and written as
// they always were; WaveFullHeight is new beside WaveReflection.
static void SeekBarKeys(bool save) {
    struct Key { const wchar_t* name; int def; int* number; bool* flag; };
    const Key keys[] = {{L"Waveform", 1, nullptr, &g.waveform},
                        {L"SeekStyle", kSeekWaveform, &g.seekStyle, nullptr},
                        {L"LiveWaveform", 0, nullptr, &g.liveWaveform},
                        {L"FullLiveWaveform", 1, nullptr, &g.fullLiveWaveform},
                        {L"WaveReflection", 1, nullptr, &g.waveReflection},
                        {L"WaveFullHeight", 1, nullptr, &g.waveFullHeight}};
    for (const Key& k : keys) {
        if (save) IniWrite(L"Window", k.name, std::to_wstring(k.number ? *k.number : (int)*k.flag));
        else if (k.number) *k.number = std::clamp(IniInt(L"Window", k.name, k.def), 0, kSeekStyles - 1);
        else *k.flag = IniInt(L"Window", k.name, k.def) != 0;
    }
}

void LoadTweaks() {
    SeekBarKeys(false);
    for (const Tweak& t : kTweaks) {
        if (t.flags & (kTweakLegacyLoad | kTweakOutside)) continue;
        if (!TweakHasOwnIniRow(t)) continue;
        double v = t.type == TweakType::Float ? IniFloat(t.section, t.iniKey, t.def)
                                              : (double)IniInt(t.section, t.iniKey, (int)t.def);
        if (!std::isfinite(v)) v = t.def;
        if (t.type == TweakType::Bool) v = v != 0;
        t.put(std::clamp(v, (double)t.min, (double)t.max));
    }
}

void SaveTweaks() {
    SeekBarKeys(true);
    for (const Tweak& t : kTweaks) {
        if (t.flags & kTweakOutside) continue;
        // The layout, library-size, Apollo and foobar2000 rows are written by the record their
        // feature owns (SaveMainLayout, SaveBrowserSettings, SaveApollo, the Album List combo), not
        // as one key per row, so a profile never has two copies to reconcile.
        if (!TweakHasOwnIniRow(t)) continue;
        // Speed, tempo and the reverb-mix preset are session rates: their rows name the menu's own
        // choice, and LoadPreferences starts the next session at normal. Writing them would imply
        // they persist, so they stay out of the file like the engine's own session state.
        if (t.flags & kTweakLegacyLoad && (!strcmp(t.key, "playback.speed") || !strcmp(t.key, "playback.tempo") ||
                                           !strcmp(t.key, "effects.reverb_mix_choice")))
            continue;
        double v = t.get();
        IniWrite(t.section, t.iniKey, t.type == TweakType::Float ? F2S((float)v) : std::to_wstring((int)v));
    }
}

bool PreferencesDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg); }

std::vector<PreferenceLink> PreferenceLinks() {
    std::vector<PreferenceLink> links;
    for (const Page& page : kPages)
        for (const Link& link : page.links)
            if (link.label && (link.command != IDM_RG_CANCEL || (g.rgRunning && !g.rgAuto)))
                links.push_back({link.command, link.label, page.name});
    return links;
}

void ShowPreferences(const char* key) {
    const Tweak* wanted = key ? FindTweak(key) : nullptr;
    if (!w.hwnd) {
        RegisterAppClass(L"CordinalPreferences", PreferencesProc);
        RECT r{0, 0, S(900), S(660)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
        RECT owner;
        GetWindowRect(g.hwnd, &owner);
        int width = r.right - r.left, height = r.bottom - r.top;
        CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"CordinalPreferences", L"Preferences",
                        WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                        owner.left + (owner.right - owner.left - width) / 2, owner.top + S(60), width, height, g.hwnd,
                        nullptr, g.inst, nullptr);
        if (!w.hwnd) return;
    }
    int keep = w.current;
    if (wanted) {  // open on the setting's own page, with nothing hiding it
        keep = (int)(wanted - kTweaks);
        SetWindowTextW(w.search, L"");
        SendMessageW(w.filter, CB_SETCURSEL, 0, 0);
        w.page = PageOf(*wanted);
        SendMessageW(w.pages, LB_SETCURSEL, w.page, 0);
    }
    Rebuild(keep);
    ShowWindow(w.hwnd, SW_SHOW);
    SetForegroundWindow(w.hwnd);
    // The asked-for setting's own control, else the page list to choose from.
    const int row = wanted ? RowOf(keep) : -1;
    SetFocus(row >= 0 ? w.rows[row].control : w.pages);
}

}  // namespace app
