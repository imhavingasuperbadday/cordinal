// Radio directory parsing, safe query construction and atomic favorites persistence.
#include "selftest_common.h"
#include "radio.h"
#include "radio_browser.h"

void TestRadioDirectory(const std::wstring& scratch) {
    const std::string json = R"([
        {"stationuuid":"station-1","name":"Indie & Rock","url_resolved":"https://radio.example/stream",
         "favicon":"https://radio.example/icon.png","countrycode":"US","tags":"indie,rock","codec":"MP3",
         "bitrate":128,"hls":0},
        {"stationuuid":"station-2","name":"Other","url":"http://example.test/live","favicon":"bad icon",
         "countrycode":"GB","tags":"rock","codec":"AAC","bitrate":64,"hls":1},
        {"stationuuid":"invalid","name":"Unsafe","url":"file:///not-a-station"}
    ])";
    std::vector<radio::Station> stations;
    bool parsed = radio::ParseStations(json, stations);
    Check(parsed && stations.size() == 2 && stations[0].name == "Indie & Rock" && stations[0].bitrate == 128 &&
          stations[1].hls && stations[1].icon.empty(), L"directory results keep usable streams and tolerate missing artwork");
    const std::string query = radio::SearchPath({"rock & roll", "indie/pop", "US", 100});
    Check(query.find("name=rock%20%26%20roll") != query.npos && query.find("tag=indie%2Fpop") != query.npos &&
          query.find("countrycode=US") != query.npos && query.find("offset=100") != query.npos,
          L"directory search escapes text and keeps genre, country and page filters");
    std::string localQuery = radio::SearchPath({"98.5 FM", "", "United States", 0, "IN"});
    Check(localQuery.find("countrycode=US") != localQuery.npos && localQuery.find("state=Indiana") != localQuery.npos &&
              localQuery.find("name=98.5") != localQuery.npos,
          L"broadcast searches accept FM numbers, U.S. country names and a state filter");
    radio::Station local{"local", "Rock 98.5 WMYK - Kokomo", "https://example.test/live", {}, "US", "rock", "MP3", 128, false, "Indiana"};
    Check(radio::StationMatches(local, {"98.5 FM", {}, "usa", 0, "Indiana"}) &&
              radio::StationMatches(local, {"WMYK-FM", {}, "US", 0, {}}) &&
              !radio::StationMatches(local, {"98.5", {}, "US", 0, "Texas"}),
          L"local broadcast results match frequencies and call signs without leaking across states");
    const std::wstring profile = scratch + L"\\profile";
    if (Ready(CreateDirectoryW(profile.c_str(), nullptr) != 0, L"radio profile")) {
        radio::Browser browser(profile);
        bool added = browser.ToggleFavorite(local) && browser.Favorite(local) && browser.Favorites().size() == 1;
        radio::Browser reopened(profile);
        Check(added && reopened.Favorites().size() == 1 && reopened.Favorites()[0].state == "Indiana" &&
                  reopened.ToggleFavorite(local) && reopened.Favorites().empty(),
              L"a favorite survives reopening and can be removed");
    }
    if (!Ready(parsed && stations.size() == 2, L"radio station records for persistence")) return;
    const std::wstring path = scratch + L"\\stations.json";
    std::vector<radio::Station> saved;
    bool roundtrip = radio::SaveFavorites(path, stations) && radio::LoadFavorites(path, saved) && saved.size() == 2 &&
                     saved[0].id == stations[0].id && saved[0].url == stations[0].url && saved[1].hls;
    Check(roundtrip, L"station favorites survive a save and reopen");
    std::string before = ReadTestBytes(path);
    stations[0].url = "https://example.test/\r\nInjected: header";
    Check(!radio::SaveFavorites(path, stations) && ReadTestBytes(path) == before,
          L"a rejected favorites write leaves the original byte-identical");
    auto original = saved;
    if (!Ready(WriteTestBytes(path, "[{\"name\":\"unfinished\"}"), L"invalid favorites file")) return;
    Check(!radio::LoadFavorites(path, saved) && saved.size() == original.size() && saved[0].id == original[0].id,
          L"an invalid favorites file does not replace the in-memory list");
}
