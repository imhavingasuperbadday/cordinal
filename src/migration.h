// The stable, readable part of a backup. Files use UTF-8 CSV, with named columns and UTC dates.
#pragma once
#include "library_archive.h"
#include "tagwriter.h"

namespace migration {
struct File {
    std::wstring path;
    std::string bytes;
};
struct PathMap {
    std::wstring from, to;
};
std::wstring RemapPath(std::wstring path, const std::vector<PathMap>& maps);
bool RemapLibrary(LibraryArchive& data, const std::vector<PathMap>& maps, std::wstring& error);
std::vector<File> Export(const LibraryArchive& data);
bool Import(const std::vector<File>& files, LibraryArchive& out, std::wstring& error);
// Statistics are a subset of the backup's files and can also be imported on their own.
std::string StatisticsCsv(const Library& library);
std::string HistoryCsv(const LibraryArchive& data);
struct Statistics {
    std::wstring path;
    uint32_t count = 0, skips = 0, added = 0, first = 0, last = 0;
    uint8_t rating = 0;
    bool hasCount = false, hasSkips = false, hasRating = false;
};
// A track's rating and play count as file tags, so they travel with the music. Fields cordinal has
// no value for are left as the file has them.
TagEdit StatisticsTags(const Track& t);
bool ReadStatisticsCsv(std::string_view bytes, std::vector<Statistics>& out, std::wstring& error);
// The XML reader is Windows' XmlLite; no external entities or network access are allowed.
bool ReadFoobarStatistics(const std::wstring& file, std::vector<Statistics>& out, size_t& unlocated,
                          std::wstring& error);
struct StatisticsPlan {
    std::vector<std::pair<TrackId, Statistics>> matched;
    size_t unmatched = 0, conflicts = 0;
};
StatisticsPlan PlanStatistics(const Library& library, const std::vector<Statistics>& rows,
                              const std::vector<PathMap>& maps);
// Counts merge by maximum, dates by earliest/latest, ratings only fill an unrated track.
// Applying the same import twice is harmless; no listening events are invented.
void ApplyStatistics(Library& library, const StatisticsPlan& plan);
} // namespace migration
