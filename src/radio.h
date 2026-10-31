// Radio Browser station records, discovery queries and locally saved favorites. Network requests
// are made by the caller's worker; the UI owns the resulting station lists.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace radio {
struct Station {
    std::string id, name, url, icon, country, tags, codec;
    unsigned bitrate = 0;
    bool hls = false;
    std::string state;
};

struct Search {
    std::string name, genre, country;
    unsigned offset = 0;
    std::string state;
};

bool ParseStations(std::string_view json, std::vector<Station>& stations);
std::string SearchPath(const Search& search);
std::string RadioCountry(std::string value);
bool StationMatches(const Station& station, const Search& search);
// Music stations whose broadcasters stream on their own sites but which Radio Browser omits.
const std::vector<Station>& BuiltInStations();
// Uses Radio Browser's DNS-discovered mirrors, retrying another when one is unavailable.
bool Discover(const Search& search, std::vector<Station>& stations, std::atomic<bool>& cancel);
bool CountPlay(const std::string& stationId, std::atomic<bool>& cancel);
// Favorites are stored as UTF-8 in the profile and survive directory outages. A failed read or
// write never replaces the caller's list or an existing file.
bool LoadFavorites(const std::wstring& path, std::vector<Station>& stations);
bool SaveFavorites(const std::wstring& path, const std::vector<Station>& stations);
}  // namespace radio
