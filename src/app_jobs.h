// Declarations of the background jobs and their UI-side handlers, implemented in app_workers.cpp
// (tag reading, ReplayGain) and app_memory.cpp (compaction). The worker entry points run off the UI
// thread; the Accept*/On*/Finish* functions run on it and consume the workers' posted payloads.
#pragma once
#include "app_state.h"

namespace app {

bool ScanInProgress();

void QueueTags(const std::vector<TrackId>& ids);
void AcceptTagBatch(std::vector<TagResult>* results);
void TagWorker(HWND hwnd);
float RgGainFor(TrackId id);
float RgGainForEstimate(TrackId id, double lufs);
void ReplayGainEqTick();
void RefreshGains(bool includePlaying = true);
void RgWorker(HWND hwnd, std::vector<TrackPath> items, std::vector<std::pair<uint64_t, uint64_t>> sizeAndTime,
              unsigned generation, bool background);
void UpdateAlbumGains(const std::vector<TrackId>& ids);
// Compaction remaps the saved measurements; only their derived cache needs resetting then.
void ForgetAlbumDynamics(bool keepMeasurements = false);
void ScanReplayGain(const std::vector<TrackId>& ids);
void MaybeAutoMeasure(bool priority);
// Bounded background batch: upcoming tracks first, then their albums and the library.
std::vector<TrackId> AutoMeasureTracks(const std::vector<TrackId>& upcoming, size_t limit);
void CancelReplayGain();
void ClearReplayGain(const std::vector<TrackId>& ids);
void WriteReplayGainTags(const std::vector<TrackId>& ids);
// Ratings and play counts into the files' tags (migration::StatisticsTags), journaled for undo.
void WriteStatisticsTags(const std::vector<TrackId>& ids);
void AddPaths(const std::vector<std::wstring>& inputs, bool playFirst);
void LoadArtFor(TrackId id, bool refresh = false);
void AppendFiles(int playlist, const std::vector<std::wstring>& files, bool playFirst);
void OnImportResult(LPARAM lp);
bool NoteUnwatchedFolders(const std::vector<std::wstring>& folders);
void WatchImportedFolders();
void OnArtResult(LPARAM lp);
void StopBackgroundJobs();
void CompactLibrary();
void FinishReplayGain(WPARAM wp, LPARAM lp);

}  // namespace app
