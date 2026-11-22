// Discover outside my library, offline: MusicBrainz relations, the unowned release and track
// choosers, the occasional-offer rule, the pick flow over canned replies, and keeping a preview.
#include "selftest_common.h"

#include "discovery.h"
#include "tagwriter.h"
#include "webfetch.h"

namespace {

constexpr const char* kSeed = "aaaaaaaa-1111-4111-8111-111111111111";
constexpr const char* kRelated = "bbbbbbbb-1111-4111-8111-111111111111";
constexpr const char* kGroup = "cccccccc-1111-4111-8111-111111111111";
constexpr const char* kRelease = "dddddddd-1111-4111-8111-111111111111";

online::Reply Reply(const char* body, unsigned status = 200) {
    online::Reply reply;
    reply.status = status;
    reply.body = body;
    return reply;
}

bool Contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

}  // namespace

void TestDiscovery(const std::wstring& scratch) {
    const auto relations = discovery::ParseArtistRelations(
        R"({"relations":[
            {"type":"member of band","artist":{"id":"bbbbbbbb-1111-4111-8111-111111111111","name":"Related"}},
            {"type":"url","url":{"resource":"https://example.test"}},
            {"type":"member of band","artist":{"id":"bbbbbbbb-1111-4111-8111-111111111111","name":"Related"}},
            {"type":"collaboration","artist":{"id":"not-an-id","name":"Broken"}},
            {"type":"collaboration","artist":{"id":"eeeeeeee-1111-4111-8111-111111111111","name":"Other"}}]})");
    Check(relations.size() == 2 && relations[0].name == "Related" && relations[0].mbid == kRelated &&
              relations[1].name == "Other" && relations[0].type == "member of band",
          L"artist relations keep artists once, skip URL and invalid rows, and keep their type");

    discovery::Snapshot owned;
    owned.Add("Seed Artist", "Owned Album", "Owned Song");
    owned.Add("Library Friend", "Their Album", "Their Song");
    Check(owned.HasArtist("seed artist") && owned.HasAlbum("Seed Artist", "Owned Album") &&
              owned.HasTrack("seed artist", "owned song") && !owned.HasArtist("Related"),
          L"the library snapshot compares artists, albums and songs loosely");

    Check(discovery::ChooseRelatedArtist(relations, owned) == 0 &&
              discovery::ChooseRelatedArtist({{"x", "Library Friend", "member of band"}}, owned) < 0,
          L"a related artist already in the library is not offered again");
    Check(discovery::ChooseRelatedArtist({}, owned) < 0, L"no relations means no related pick");

    std::vector<online::ReleaseGroup> groups = {
        {kGroup, "Owned Album", "Album", 2001, "2001-01-01", false},
        {"11111111-1111-4111-8111-111111111111", "New Album", "Album", 2024, "2024-05-01", false},
        {"22222222-1111-4111-8111-111111111111", "A Single", "Single", 2023, "2023-01-01", false},
        {"33333333-1111-4111-8111-111111111111", "Greatest Hits", "Compilation", 2020, "", false},
    };
    Check(discovery::ChooseRelease(groups, "Seed Artist", owned) == 1,
          L"an unowned album comes before bits, singles and compilations");
    Check(discovery::ChooseRelease({groups[0]}, "Seed Artist", owned) < 0 &&
              discovery::ChooseRelease({groups[3]}, "Seed Artist", owned) < 0,
          L"an owned album or a compilation alone offers nothing");

    radar::Album album;
    album.title = "New Album";
    album.artist = "Seed Artist";
    album.releaseId = kRelease;
    album.tracks = {
        {"Owned Song", "Seed Artist", 1, 1, 200},
        {"Fresh Song", "Seed Artist", 1, 2, 240},
        {"Intro", "Seed Artist", 1, 3, 12},
    };
    Check(discovery::ChooseTrack(album, "Seed Artist", owned) == 1,
          L"a song the library has, and a scrap under 20 seconds, are not offered");
    Check(discovery::ChooseTrack({album.title, album.artist, album.date, album.releaseId,
                                  {{"Owned Song", "Seed Artist", 1, 1, 200}}},
                                 "Seed Artist", owned) < 0,
          L"a release whose every song is owned offers nothing");

    Check(!discovery::ShouldDiscover(false, true, true, false, false, 99) &&
              !discovery::ShouldDiscover(true, false, true, false, false, 99) &&
              !discovery::ShouldDiscover(true, true, false, false, false, 99) &&
              !discovery::ShouldDiscover(true, true, true, true, false, 99),
          L"discovery stays off unless the preference, radio and local playback are all on, with nothing pending");
    Check(!discovery::ShouldDiscover(true, true, true, false, false, 1) &&
              discovery::ShouldDiscover(true, true, true, false, false, discovery::kFirstAfter) &&
              !discovery::ShouldDiscover(true, true, true, false, true, discovery::kBetween - 1) &&
              discovery::ShouldDiscover(true, true, true, false, true, discovery::kBetween),
          L"the first offer waits a couple of local tracks, later ones longer, so discovery stays occasional");

    auto fetch = [](const std::string& url) {
        if (url.find("ws/2/artist?") != std::string::npos)
            return Reply(R"({"artists":[{"id":"aaaaaaaa-1111-4111-8111-111111111111","name":"Seed Artist","score":100}]})");
        if (url.find("inc=artist-rels") != std::string::npos)
            return Reply(R"({"relations":[{"type":"member of band","artist":{"id":"bbbbbbbb-1111-4111-8111-111111111111","name":"Related"}}]})");
        if (url.find("release-group?artist=" + std::string(kRelated)) != std::string::npos)
            return Reply(R"({"release-group-count":1,"release-groups":[
                {"id":"cccccccc-1111-4111-8111-111111111111","title":"New Album","primary-type":"Album",
                 "first-release-date":"2024-05-01"}]})");
        if (url.find("ws/2/release?release-group=" + std::string(kGroup)) != std::string::npos)
            return Reply(R"({"releases":[{"id":"dddddddd-1111-4111-8111-111111111111","title":"New Album",
                "date":"2024-05-01","artist-credit":[{"name":"Related"}],
                "release-group":{"id":"cccccccc-1111-4111-8111-111111111111"},"media":[
                {"position":1,"tracks":[
                  {"position":1,"title":"Owned Song","length":200000},
                  {"position":2,"title":"Fresh Song","length":240000}]}]}]})");
        return Reply("", 404);
    };
    online::Cancel cancel;
    discovery::Track pick;
    const bool found = discovery::Find("Seed Artist", {}, owned, cancel, pick, 7, fetch);
    Check(found && pick.title == "Fresh Song" && pick.artist == "Related" && pick.album == "New Album" &&
              pick.releaseId == kRelease && pick.releaseGroup == kGroup && pick.trackNo == 2 &&
              pick.trackTotal == 2 && pick.durationSec == 240 && pick.site == "musicbrainz",
          L"the pick flow reads a related artist's new album and chooses the song the snapshot lacks");

    discovery::Snapshot allOwned;
    allOwned.albums = {"related\x1f" "new album"};
    allOwned.tracks = {"related\x1f" "owned song", "related\x1f" "fresh song"};
    Check(!discovery::Find("Seed Artist", {}, allOwned, cancel, pick, 7, fetch),
          L"when the library already has every song, nothing is offered");

    // Keeping a preview copies it into Artist\Album with its number, never over another file.
    const std::wstring root = scratch + L"\\discovery";
    const std::wstring preview = root + L"\\preview\\song.m4a";
    const std::wstring library = root + L"\\library";
    const std::wstring data = root + L"\\data";
    Ready(SHCreateDirectoryExW(nullptr, (root + L"\\preview").c_str(), nullptr) == ERROR_SUCCESS &&
              SHCreateDirectoryExW(nullptr, data.c_str(), nullptr) == ERROR_SUCCESS,
          L"make the preview and data folders");
    Ready(WriteTestBytes(preview, "a tagged preview"), L"write the preview the keep step copies");
    WebTrack track;
    track.site = "musicbrainz";
    track.id = std::string(kRelease) + "#1.2";
    track.title = "Fresh Song";
    track.artist = "Related";
    track.album = "New Album";
    track.ext = "m4a";
    track.trackNo = 2;
    std::wstring kept;
    std::string error;
    Check(WebFetchKeep(track, preview, library, data, kept, error) && kept.ends_with(L"02 Fresh Song.m4a") &&
              ReadTestBytes(kept) == "a tagged preview" && GetFileAttributesW(preview.c_str()) != INVALID_FILE_ATTRIBUTES,
          L"keeping a preview copies its tagged file into the library and leaves the preview playing");
    std::wstring second;
    Check(WebFetchKeep(track, preview, library, data, second, error) && second != kept &&
              ReadTestBytes(second) == "a tagged preview",
          L"keeping the same song twice numbers the second copy instead of replacing the first");
    const std::string record = ReadTestBytes(WebFetchRecordPath(data));
    Check(Contains(record, "musicbrainz") && Contains(record, "Fresh Song"),
          L"the kept file is in the saved record, so a later save sees it as yours");
}
