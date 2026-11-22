// Spotify export and CSV import parsing, the library split and the assembly that rebuilds a
// list's order, all against fabricated files with no network and no library: the parsers take
// text, the split takes an OwnedIndex, the assembly takes finished queue items.
#include "selftest_common.h"
#include "playlist_import.h"

#include <string>
#include <vector>

namespace {

// Spotify's account data export, as "Download your data" writes it: saved tracks with their
// artist as an object, a local file the service only remembered, accented names, and a saved
// album that only the site's own page can list.
const char kYourLibrary[] = R"json({"tracks":[
 {"uri":"spotify:track:aaa111","name":"Song One","artist":{"name":"Mira"},"album":{"uri":"spotify:album:alb1","name":"First"},"duration_ms":201000},
 {"uri":"spotify:local:C:\\music\\home.mp3","name":"Home Recording","artist":{"name":"Mira"},"duration_ms":100000},
 {"uri":"spotify:track:bbb222","name":"Déjà Vu","artist":{"name":"Sun & Rain"},"album":{"name":"Second"},"duration_ms":180500}
],"albums":[
 {"uri":"spotify:album:alb9","name":"Saved Album","artist":{"name":"Mira"}},
 {"uri":"spotify:local:x","name":"Not A Link","artist":{"name":"Mira"}}
]})json";

// The playlists file: private and large lists with every track, a removed entry, a podcast
// episode, a local file, an unnamed list, and a track whose artists are a list.
const char kPlaylist1[] = R"json({"playlists":[
 {"name":"Road Trip","items":[
  {"track":{"uri":"spotify:track:t1","name":"Opening","artists":[{"name":"A One"},{"name":"B Two"}],"album":{"name":"Album A","uri":"spotify:album:x"},"duration_ms":183000}},
  {"track":null},
  {"episode":{"uri":"spotify:episode:e1","name":"A Podcast"}},
  {"track":{"uri":"spotify:local:foo","name":"Local","artists":[{"name":"A One"}],"duration_ms":1000}},
  {"track":{"uri":"spotify:track:t2","name":"Closing","artists":[{"name":"C Three"}],"album":{"name":"Album B"},"duration_ms":61000}}
 ]},
 {"name":"","items":[
  {"track":{"uri":"spotify:track:t3","name":"Untitled List Song","artists":[{"name":"D Four"}],"duration_ms":99999}}
 ]}
]})json";

// An Exportify-style CSV: a BOM, CRLF, quoted commas and doubled quotes, a missing duration,
// a missing uri, a clock instead of milliseconds, and a blank row.
const char kCsv[] =
    "\xEF\xBB\xBF"
    "Track Name,Artist Name(s),Album Name,Duration (ms),Track URI\r\n"
    "\"Sunset, Pt. 2\",Mira,First,212000,spotify:track:777\r\n"
    "No Clock,Mira,,,\n"
    "\"Quoted \"\"Title\"\"\",Other Artist,Second,3:45,https://open.spotify.com/track/888?si=x\n"
    ",,,,\n";

// One CSV holding several lists, the way a move service writes them.
const char kCsvNames[] =
    "Playlist Name,Track Name,Artist Name(s),Duration (ms)\n"
    "Morning,First Song,Artist A,100000\n"
    "Morning,Second Song,Artist A,100000\n"
    "Evening,Late Song,Artist B,200000\n";

WebTrack Missing(const char* artist, const char* title) {
    WebTrack t;
    t.site = "spotify";
    t.artist = artist;
    t.title = title;
    return t;
}

WebFetchItem Item(WebState state, const char* artist, const char* title, const wchar_t* file) {
    WebFetchItem it;
    it.track = Missing(artist, title);
    it.state = state;
    it.file = file ? file : L"";
    return it;
}

}  // namespace

void TestPlaylistImport() {
    testlog::Batch batch;

    batch.Next(L"YourLibrary.json reads the saved tracks as one Liked Songs list, skips the "
               L"service's memories of local files, and turns saved albums into site links");
    {
        import::ImportList liked;
        std::vector<std::string> albumLinks;
        Check(import::ParseYourLibrary(kYourLibrary, liked, albumLinks),
              L"the export parses");
        if (Ready(liked.tracks.size() == 2, L"the saved tracks read")) {
            Check(liked.title == "Liked Songs", L"the list carries Spotify's own name for it");
            Check(liked.favorites, L"the saved tracks are hearts: the import marks them so");
            Check(liked.skipped == 1, L"the local file is one skipped row");
            Check(liked.tracks[0].uri == "spotify:track:aaa111" && liked.tracks[0].artist == "Mira" &&
                      liked.tracks[0].album == "First" && liked.tracks[0].durationMs == 201000,
                  L"a row keeps its own uri, artist, album and clock");
            Check(liked.tracks[1].title == "Déjà Vu" && liked.tracks[1].artist == "Sun & Rain",
                  L"accented names arrive as they were, UTF-8 end to end");
        }
        Check(albumLinks.size() == 1 && albumLinks[0] == "https://open.spotify.com/album/alb9",
              L"a saved album becomes the site link that names its tracks; a local one does not");
    }

    batch.Next(L"Playlist1.json reads every playlist however private, keeps the order, and "
               L"skips what the queue could not use");
    {
        std::vector<import::ImportList> lists;
        Check(import::ParsePlaylist1(kPlaylist1, lists) && lists.size() == 2, L"both playlists read");
        if (Ready(lists.size() == 2, L"the playlists")) {
            Check(lists[0].title == "Road Trip" && lists[0].tracks.size() == 2 && lists[0].skipped == 3,
                  L"the removed entry, the episode and the local file are skipped, counted");
            Check(lists[0].tracks[0].artist == "A One, B Two",
                  L"a track's artist list is written as the move services write one name");
            Check(lists[0].tracks[1].title == "Closing" && lists[0].tracks[1].durationMs == 61000,
                  L"the second track keeps its own place, name and clock");
            Check(lists[1].title == "Spotify playlist" && lists[1].tracks.size() == 1,
                  L"an unnamed list still reads, under a name of its own");
        }
    }

    batch.Next(L"CSV playlists parse with a BOM, quoted commas, doubled quotes, missing clocks "
               L"and uris, and a Playlist Name column splits one file into several lists");
    {
        std::vector<import::ImportList> lists;
        Check(import::ParseCsv(kCsv, "Pasted", lists) && lists.size() == 1, L"the CSV reads");
        if (Ready(lists.size() == 1 && lists[0].tracks.size() == 3, L"the CSV rows")) {
            Check(lists[0].title == "Pasted", L"a row without a playlist names takes the file's title");
            Check(lists[0].tracks[0].title == "Sunset, Pt. 2" && lists[0].tracks[0].artist == "Mira" &&
                      lists[0].tracks[0].durationMs == 212000 && lists[0].tracks[0].uri == "spotify:track:777",
                  L"a quoted comma stays inside the field, and the row keeps its clock and uri");
            Check(lists[0].tracks[1].title == "No Clock" && lists[0].tracks[1].durationMs == 0 &&
                      lists[0].tracks[1].uri.empty(),
                  L"missing cells are simply missing, not errors");
            Check(lists[0].tracks[2].title == "Quoted \"Title\"" && lists[0].tracks[2].durationMs == 225000,
                  L"doubled quotes unescape and a clock reads as minutes and seconds");
            Check(lists[0].skipped == 1, L"the blank row is counted, not imported");
        }
        std::vector<import::ImportList> named;
        Check(import::ParseCsv(kCsvNames, "Pasted", named) && named.size() == 2,
              L"a Playlist Name column splits the file into one list per name");
        if (Ready(named.size() == 2, L"the named lists")) {
            Check(named[0].title == "Morning" && named[0].tracks.size() == 2 &&
                      named[1].title == "Evening" && named[1].tracks.size() == 1,
                  L"each list keeps its own rows, in the file's order");
        }
    }

    batch.Next(L"pasted text says what it is: an export or a CSV is an import, links and prose "
               L"are not, and a file that is neither is refused with words");
    {
        testlog::Cases sniff;
        sniff.Add(import::LooksLikeImport(kYourLibrary), L"the library export");
        sniff.Add(import::LooksLikeImport(kPlaylist1), L"the playlist export");
        sniff.Add(import::LooksLikeImport(kCsv), L"a CSV list");
        sniff.Add(!import::LooksLikeImport("https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWqT"),
                  L"a Spotify link");
        sniff.Add(!import::LooksLikeImport("some words, with commas, but no columns"), L"prose");
        sniff.Add(!import::LooksLikeImport(""), L"nothing");
        Check(sniff.Passed(), L"only exports and CSV lists look like imports: %ls", sniff.failed.c_str());

        std::vector<import::ImportList> lists;
        std::vector<std::string> albumLinks;
        std::string error;
        Check(!import::ParseImportText("{\"shape\": true}", L"X", lists, albumLinks, error) &&
                  !error.empty(),
              L"a JSON file that is not an export is refused in words");
        Check(!import::ParseImportText("a,b\n1,2\n", L"X", lists, albumLinks, error) && !error.empty(),
              L"a CSV without a track column is refused in words");
        Check(import::ParseImportText(kPlaylist1, L"X", lists, albumLinks, error) && lists.size() == 2,
              L"the import entry point reads what the sniff found");
    }

    batch.Next(L"the library split keeps the rows the user already owns, and sends the rest as "
               L"Spotify-style rows the queue can match, with stable ids for re-imports");
    {
        import::OwnedIndex owned;
        owned.Add("A One", "Opening", L"C:\\Music\\opening.mp3");
        import::ImportList list;
        list.title = "Road Trip";
        list.favorites = true;
        list.tracks.push_back({"A One, B Two", "Opening", "Album A", "spotify:track:t1", 183000});
        list.tracks.push_back({"C Three", "Closing", "Album B", "", 61000});
        std::vector<import::ImportList> lists{list};
        import::ImportPlan plan;
        import::SplitByLibrary(lists, owned, plan);
        Check(plan.lists.size() == 1 && plan.lists[0].slots.size() == 2, L"both rows are planned");
        if (Ready(plan.lists.size() == 1 && plan.lists[0].slots.size() == 2, L"the plan")) {
            const import::ImportPlanList& planned = plan.lists[0];
            Check(planned.favorites, L"the plan remembers that the list's rows are hearts");
            Check(planned.slots[0].missing == -1 && planned.slots[0].owned == L"C:\\Music\\opening.mp3",
                  L"a row the library owns keeps its file, case and spacing forgiven");
            Check(planned.slots[1].missing == 0 && planned.missing.size() == 1,
                  L"a row the library lacks goes to the queue");
            Check(planned.missing[0].id == "name:C Three\x1f" "Closing" && planned.missing[0].durationSec == 61,
                  L"a row without a uri gets the stable name key the record remembers");
        }
        // The id a uri names is the record's key, so a re-import skips what was saved.
        std::vector<import::ImportList> uriLists;
        import::ImportList uriList;
        uriList.tracks.push_back({"A One", "Different Name", "Album A", "spotify:track:t9", 100000});
        uriLists.push_back(uriList);
        import::ImportPlan uriPlan;
        import::SplitByLibrary(uriLists, owned, uriPlan);
        Check(uriPlan.lists[0].missing[0].id == "t9",
              L"a row with a uri is keyed by the site's own id for it");
    }

    batch.Next(L"the assembly rebuilds a list's exact order from what the run produced: saved "
               L"and owned files in place, failed matches held by the library's copy, the rest "
               L"skipped without moving anything");
    {
        import::OwnedIndex owned;
        owned.Add("Mira", "Failed But Owned", L"C:\\Music\\owned.mp3");
        std::vector<WebFetchItem> items;
        items.push_back(Item(WebState::Done, "Mira", "First", L"C:\\Lib\\first.mp3"));
        items.push_back(Item(WebState::Exists, "Mira", "Second", L"C:\\Lib\\second.mp3"));
        items.push_back(Item(WebState::Failed, "Mira", "Failed But Owned", nullptr));
        items.push_back(Item(WebState::Failed, "Mira", "Failed And Gone", nullptr));
        items.push_back(Item(WebState::Resolving, "Mira", "Cancelled Midway", nullptr));
        import::ImportSlot ownedSlot;
        ownedSlot.owned = L"C:\\Music\\before.mp3";
        std::vector<import::ImportSlot> slots{ownedSlot};
        const std::vector<import::ImportSlot> range = import::RangeSlots(items.size());
        slots.insert(slots.end(), range.begin(), range.end());
        const std::vector<std::wstring> files = import::AssembleFiles(items, 0, slots, owned);
        Check(files.size() == 4 &&
                  files[0] == L"C:\\Music\\before.mp3" && files[1] == L"C:\\Lib\\first.mp3" &&
                  files[2] == L"C:\\Lib\\second.mp3" && files[3] == L"C:\\Music\\owned.mp3",
              L"owned, saved, saved and library-held files keep their exact order, the rest drop "
              L"out without shifting any row");
    }
}
