// An owned library snapshot for backup workers and validation before restoring a profile.
#pragma once
#include "library.h"
#include "playlog.h"
#include "points.h"

struct LibraryArchive {
    Library library;
    std::vector<Playlist> playlists;
    std::vector<PlayEvent> log;
    PointsState points;
    bool hasPoints = false;
    std::map<AlbumId, AlbumMeasurement> albums;
};

LibraryArchive CaptureLibraryArchive(const std::vector<Playlist>& playlists);
bool ReadLibraryArchive(const std::wstring& file, LibraryArchive& out);
bool WriteLibraryArchive(const std::wstring& file, const LibraryArchive& data, std::wstring& error);
