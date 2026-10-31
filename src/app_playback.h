// Playback control shared by the application modules: the queue and play order, the now-playing
// strip and status bar, the seek bar's waveform and its looped sections, and play statistics.
// app_playback.cpp holds control/order/stats, app_waveform.cpp the waveform, app_region.cpp the
// sections. All of it runs on the UI thread; engine events arrive through OnEngineEvent.
#pragma once
#include "app_state.h"

namespace app {

void PlayList(const std::vector<TrackId>& items, int index, double start = 0, bool paused = false);
void QueueTracks(const std::vector<TrackId>& ids, bool next);
void ClearQueue();
int QueuePosition(TrackId id);
int SnapshotToNowPlaying(int pl, int& index);
void TrackListening();
void FinishListening(bool manual);
// `when` is the unix time the listen ended, for one reported later (a phone's offline play); 0 = now.
void RecordListening(TrackId id, double seconds, bool manual, uint32_t when = 0);
void DrawStatusItems(HDC dc, RECT r);
void ClickStatusItem(int item);
LRESULT CALLBACK StatusSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);
PlayItem MakeItem(unsigned long long token, TrackId id);
unsigned long long NewToken(TrackId id, int pl, uint32_t index = UINT32_MAX);
int ComputeNext(unsigned long long fromToken, bool manual, int& plOut);
std::vector<TrackId> UpcomingTracks();
void AnswerNext(unsigned long long token);
void RefreshNext();
void PlayTrack(int pl, int index, bool remember, double start = 0, bool paused = false);
void PlayRow(int row);
void PlayFocusedOrFirst();
void Stop();
void Next();
void Previous();
void PlayButton();
void TogglePause();
bool Resume();
// Reopens the last session's track, paused where it stopped or playing at once, as Preferences asks.
void ReopenLastTrack();
void SetVolumePercent(int percent);
void ToggleMute();
void ApplyEffects();  // reverb and smart volume to the engine
void SetSpeed(double speed);
void SetTempo(double tempo);
std::wstring RateText();  // "1.25x speed", empty at normal speed and tempo
void RevealPlayingTrack();
void PlaySfx(Sfx sound, int arg = 0, bool evenWhenOff = false);
double CurrentDuration();
void RequestSeek(double seconds);
void SeekBy(double seconds);
void UpdateNowPlaying();
void UpdateStatus();
void ShowPlaybackTime(double pos, double duration);
void PaintArt(HDC dc);
void OnEngineEvent(EngineEvent e, unsigned long long token);
void PrepareWaveform(TrackId id);
void PrefetchWaveform(TrackId id);
void WaveformTrackChanged(bool refresh = false);
void OnPeaksReady(uint32_t requestId);
bool WaveformShown();
bool LiveWaveformShown();
bool DrawWaveform(HDC dc, const RECT& rc, const RECT& channel, double frac);
void WaveformTick();
bool WaveformStep(UINT_PTR id);
int WaveformPeak(double from, double to);
COLORREF WaveformPlayedColor();
COLORREF WaveformRestColor();
// The waveform's look (app_fountain.cpp): Dark Fountain at every position and per-position colors.
void LoadWaveLook();
bool FountainShown();
void PaintFountain(HDC dc, const RECT& client, int top, int bottom, double frac);
void FountainWake();
bool FountainStep(UINT_PTR id);
void AppendWaveLookMenu(HMENU wave);
void UpdateWaveLookMenu(HMENU m);
bool WaveLookCommand(int id);
// The DELTARUNE theme's sound effects (app_deltarune.cpp): false when `sound` has none.
bool PlayGameSound(Sfx sound, float gain);
// Plays a theme's sound (stereo at `rate`) on the sound effect stream, unless a game's music clip is
// playing there; false when it didn't.
bool PlayThemeClip(const std::vector<float>& stereo, unsigned rate, float gain);
// One of those sounds (an IDR_SND_* resource) as interleaved stereo; empty when it can't be decoded.
std::vector<float> LoadGameSound(int id, unsigned& rate);
// Robotic: Chapter 2's Cyber World sound, the effects bitcrushed and messages spoken in Queen's blip.
bool RoboticSounds();
void SetRoboticSounds(bool on);  // the loaded choice, without a sound or a save
void ToggleRoboticSounds();
void MessageBlip();
std::wstring CurrentChapterText();
double ChapterStartAfter(double pos);
Region CurrentRegion();
Region RegionOf(TrackId id);
void RegionTrackChanged();
bool RegionMouse(const MSG& msg);
bool ShowRegionMenu(POINT pt);
double RegionTimeAt(HWND bar, POINT pt);
bool CommandRegion(int id, double pos);
void DrawRegion(HDC dc, const RECT& rc, const RECT& channel);
void RemapOccurrences(int playlist, const std::vector<uint32_t>& mapping);
int TokenIndex(unsigned long long token, int playlist);

}  // namespace app
