// Full migration round trips, malformed input, profile rollback and real foo_playcount XML shapes.
#include "selftest_common.h"
#include "backup.h"

namespace {
std::string Settings(const std::wstring& text) {
    std::string bytes = "\xFF\xFE";
    bytes.append((const char*)text.data(), text.size() * sizeof(wchar_t));
    return bytes;
}
std::string& File(std::vector<migration::File>& files, const wchar_t* name) {
    for (auto& file : files)
        if (file.path == name) return file.bytes;
    throw std::runtime_error("missing migration fixture");
}
} // namespace

void TestMigration(const std::wstring& scratch) {
    namespace fs = std::filesystem;
    fs::create_directories(scratch);
    LibraryArchive source;
    auto a = source.library.Add(L"D:\\Music\\Bj\x00F6rk\\One.flac");
    auto b = source.library.Add(L"D:\\Music\\Two.flac|2");
    auto c = source.library.Add(L"E:\\Other\\Missing.mp3");
    auto& t = source.library[a];
    t.title = source.library.Intern("A, \"quoted\"\r\ntitle");
    t.artist = source.library.Intern("Bj\xC3\xB6rk");
    t.album = source.library.Intern("Album");
    t.albumArtist = t.artist;
    t.comment = source.library.Intern("=SUM(1,2)");
    t.label = source.library.Intern("Label");
    t.flags = Track::kInLibrary | Track::kLoaded | Track::kRgAlbumMeasured;
    t.rating = 90;
    t.playCount = 42;
    t.skipCount = 3;
    t.added = 1700000000;
    t.firstPlayed = 1700000100;
    t.lastPlayed = 1700000200;
    t.audioHash = UINT64_MAX;
    t.rgTrackGain = -750;
    t.rgAlbumGain = -500;
    t.rgAlbumPeak = 25000;
    t.rgHigh = 20;
    source.library[b].flags = Track::kInLibrary | Track::kLoaded;
    source.library[b].title = source.library.Intern("=2+2");
    source.library[c].flags = Track::kMissing | Track::kExcluded;
    source.library[c].playCount = 7;
    source.log = {{a, 1700000100, 200, PlayEvent::kCounted},
                  {a, 1700000200, 10, PlayEvent::kSkip},
                  {c, 1700000300, 100, PlayEvent::kCounted}};
    source.playlists = {{L"Library", {a, b}, PlaylistKind::Library},
                        {L"Duplicates / \x00E9", {b, a, b}, PlaylistKind::Static},
                        {L"Empty", {}, PlaylistKind::Static},
                        {L"Favourite", {a}, PlaylistKind::Auto, SerializeAutoPlaylist(AutoPlaylist{})}};
    source.playlists[1].position.search = L"a,b\nquote\"";
    source.playlists[1].position.selected = {0, 2};
    source.playlists[1].position.focus = 2;
    source.albums[AlbumKeyOf(t)] = {-500, 25000, 2, {a}};
    source.hasPoints = true;
    source.points.total = 123456;
    source.points.streakLength = 4;
    source.points.byDay = {{100, 50}, {101, 100}};
    source.points.recent = {{1700000000, PointsKind::Play, a, 123},
                            {1700000300, PointsKind::BackPay, kInvalidTrack, 333}};
    const auto files = migration::Export(source);
    LibraryArchive restored;
    std::wstring error;
    if (!Ready(migration::Import(files, restored, error), L"readable migration fixture imports")) {
        fwprintf(g_log, L"migration: %ls\n", error.c_str());
        return;
    }
    auto roundtrip = migration::Export(restored);
    bool same = files.size() == roundtrip.size();
    for (size_t i = 0; same && i < files.size(); ++i)
        same = files[i].path == roundtrip[i].path && files[i].bytes == roundtrip[i].bytes;
    Check(same, L"CSV round trip preserves all track fields, duplicate/empty/auto playlists, views, history, "
                L"points and loudness");

    LibraryArchive reopened;

    testlog::Cases invalid;
    for (const auto& [name, value] : std::vector<std::pair<std::wstring, std::string>>{
             {L"backup.ini", "[backup]\r\nformat=99\r\n"},
             {L"library.csv", "path,play_count\n\"unterminated"},
             {L"library.csv", "path,path\na,b\n"},
             {L"playlist-items.csv", "playlist_id,track_id\n999,0\n"},
             {L"history.csv", "track_id,path,played_at_utc,listened_seconds,counted,skipped\n0,x,2024-02-"
                              "30T12:00:00Z,0,1,0\n"}}) {
        auto damaged = files;
        File(damaged, name.c_str()) = value;
        const auto before = migration::StatisticsCsv(restored.library);
        invalid.Add(!migration::Import(damaged, restored, error) &&
                        before == migration::StatisticsCsv(restored.library),
                    name);
    }
    Check(invalid.Passed(),
          L"malformed CSV, unsupported versions and bad references leave the destination unchanged: %ls",
          invalid.failed.c_str());
    std::vector<migration::PathMap> maps{{L"D:\\Music", L"F:\\Audio"},
                                         {L"D:\\Music\\Bj\x00F6rk", L"G:\\Bj\x00F6rk"},
                                         {L"E:\\Other", L"H:\\Lost"}};
    Check(migration::RemapPath(L"d:/music/Bj\x00F6rk/One.flac", maps) == L"G:\\Bj\x00F6rk\\One.flac" &&
              migration::RemapPath(L"D:\\Music-old\\One.flac", maps) == L"D:\\Music-old\\One.flac" &&
              migration::RemapPath(L"D:\\Music\\Two.flac|2", maps) == L"F:\\Audio\\Two.flac|2",
          L"folder remapping uses the longest complete prefix, Unicode, mixed separators and subtrack "
          L"identity");

    if (!Ready(migration::RemapLibrary(restored, maps, error), L"migration locations remapped")) return;
    LibraryArchive collision;
    collision.library.Add(L"D:\\A\\same.flac");
    collision.library.Add(L"D:\\B\\same.flac");
    Check(!migration::RemapLibrary(collision, {{L"D:\\A", L"X:\\New"}, {L"D:\\B", L"X:\\New"}}, error) &&
              collision.library.Find(L"D:\\B\\same.flac") == 1,
          L"colliding path maps are rejected without changing either song");
    auto settings = Settings(L"[Library]\r\nFolders=D:\\Music|E:\\Other\r\n[Player]\r\nResumePath=D:"
                             L"\\Music\\Two.flac|2\r\nVolume=73\r\n");
    auto remappedIni = backup::RemapSettings(settings, maps);

    std::vector<migration::Statistics> stats;
    if (!Ready(migration::ReadStatisticsCsv(migration::StatisticsCsv(source.library), stats, error),
               L"statistics CSV read for import"))
        return;
    Library destination;
    auto dst = destination.Add(L"F:\\Audio\\Two.flac|2");
    destination[dst].playCount = 99;
    destination[dst].rating = 40;
    auto row = stats[0];
    row.path = L"D:\\Music\\Two.flac|2";
    row.count = 120;
    auto plan = migration::PlanStatistics(destination, {row}, maps);
    migration::ApplyStatistics(destination, plan);
    migration::ApplyStatistics(destination, plan);
    Check(destination[dst].playCount == 120 && destination[dst].rating == 40 &&
              destination[dst].firstPlayed == t.firstPlayed && plan.conflicts == 1,
          L"statistics import is idempotent and preserves existing ratings while importing first-play dates");
    Track rated, unrated;
    rated.rating = 90;
    rated.playCount = 7;
    const TagEdit tags = migration::StatisticsTags(rated);
    Check(tags.rating == "0.9" && tags.playCount == "7" && migration::StatisticsTags(unrated).Empty(),
          L"statistics become FMPS tags, and a track without them leaves the file's own alone");

    const auto xml = scratch + L"\\foobar.xml";
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><PlaybackStatistics Version=\"3.0\" Mapping=\"opaque\">"
        "<Entry ID=\"abc\" Count=\"42\" Rating=\"9\" RatingFriendly=\"4.5\" Added=\"133444736000000000\" "
        "FirstPlayed=\"133444737000000000\" LastPlayed=\"133444738000000000\">"
        "<Item Path=\"D:\\Music\\A &amp; B.flac\"/><Item Path=\"D:\\Music\\copy.flac\"/></Entry>"
        "<Entry ID=\"unlocated\" Count=\"2\"/></PlaybackStatistics>";
    Ready(WriteTestBytes(xml, body), L"foobar2000 XML fixture written");
    size_t unlocated = 0;
    bool xmlRead = migration::ReadFoobarStatistics(xml, stats, unlocated, error);
    Check(xmlRead && stats.size() == 2 && unlocated == 1 && stats[0].path == L"D:\\Music\\A & B.flac" &&
              stats[0].count == 42 && stats[0].rating == 90 && stats[0].first == 1700000100,
          L"foo_playcount XML imports real FILETIME dates, half-star ratings, entities and multiple Item "
          L"paths: read=%d rows=%zu unlocated=%zu error=%ls",
          xmlRead, stats.size(), unlocated, error.c_str());
    testlog::Cases xmlBad;
    for (const auto& text :
         {std::string(
              "<PlaybackStatistics><Entry Count=\"no\"><Item Path=\"x\"/></Entry></PlaybackStatistics>"),
          body.substr(0, body.size() - 4),
          std::string(
              "<!DOCTYPE PlaybackStatistics [<!ENTITY x SYSTEM \"file:///C:/secret\">]>"
              "<PlaybackStatistics><Entry Count=\"1\"><Item Path=\"&x;\"/></Entry></PlaybackStatistics>")}) {
        Ready(WriteTestBytes(xml, text), L"malformed XML fixture written");
        xmlBad.Add(!migration::ReadFoobarStatistics(xml, stats, unlocated, error),
                   FromUtf8(text.substr(0, 40)));
    }
    Check(xmlBad.Passed(),
          L"invalid and truncated XML and external entities never import partial statistics: %ls",
          xmlBad.failed.c_str());

    const auto profile = scratch + L"\\profile", folder = scratch + L"\\backup";
    fs::create_directories(profile + L"\\packs\\mine");
    Ready(WriteTestBytes(profile + L"\\packs\\mine\\pack.ini", "[pack]\nformat=1\nname=Mine\n") &&
              WriteTestBytes(profile + L"\\listenbrainz.token", "secret") &&
              WriteTestBytes(profile + L"\\settings.ini", settings),
          L"throwaway backup profile prepared");
    std::atomic<bool> cancel{false};
    if (!Ready(backup::Create(folder, profile, settings, source, cancel, error),
               L"complete backup created")) {
        fwprintf(g_log, L"backup: %ls\n", error.c_str());
        return;
    }
    std::string readIni;
    std::wstring stage, previous;
    Check(
        backup::Read(folder, reopened, readIni, error) && readIni == settings &&
            fs::exists(folder + L"\\profile\\packs\\mine\\pack.ini") &&
            !fs::exists(folder + L"\\profile\\listenbrainz.token") &&
            backup::Stage(folder, profile, remappedIni, restored, cancel, stage, error) &&
            ReadTestBytes(profile + L"\\settings.ini") == settings &&
            backup::Commit(profile, stage, previous, error) &&
            ReadTestBytes(profile + L"\\settings.ini") ==
                Settings(L"[Library]\r\nFolders=F:\\Audio|H:\\Lost\r\n[Player]\r\nResumePath=F:\\Audio\\Two."
                         L"flac|2\r\nVolume=73\r\n") &&
            ReadLibraryArchive(profile + L"\\library.cdb", reopened) &&
            reopened.library[a].firstPlayed == t.firstPlayed &&
            ReadTestBytes(previous + L"\\settings.ini") == settings &&
            ReadTestBytes(previous + L"\\listenbrainz.token") == "secret",
        L"staged restore switches the whole profile and retains every byte of the previous profile for undo");
    // Simulate a process loss after the old profile was renamed but before the new one was installed.
    const auto interrupted = scratch + L"\\interrupted", prior = interrupted + L".before-restore-test",
               staged = interrupted + L".restoring-test";
    fs::create_directories(prior);
    Ready(WriteTestBytes(prior + L"\\keep", "original") &&
              WriteTestBytes(interrupted + L".restore-journal", ToUtf8(staged) + "\n" + ToUtf8(prior)),
          L"interrupted restore fixture prepared");
    Check(backup::Recover(interrupted, error) && ReadTestBytes(interrupted + L"\\keep") == "original" &&
              !fs::exists(interrupted + L".restore-journal"),
          L"startup recovers the original profile after an interrupted restore swap");
    const auto original = ReadTestBytes(folder + L"\\statistics.csv");
    Ready(WriteTestBytes(folder + L"\\statistics.csv", original + "damaged"),
          L"corrupted backup fixture prepared");
    Check(!backup::Read(folder, reopened, readIni, error) && !error.empty(),
          L"backup checksums reject corruption before a profile can be replaced");
}
