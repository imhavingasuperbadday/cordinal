// Online metadata without the network: the shared checks of metadata_checks.h, WIC confirming the
// images the core accepts, and an embedded cover undone through one journal on real FLAC and MP3
// copies. Nothing here opens a connection; the providers are fed canned replies.
#include "selftest_common.h"

#include "metadata_checks.h"

namespace {

void Report(const metadata_checks::Group& group) {
    std::wstring failed;
    for (const auto& f : group.failures) failed += L" | " + FromUtf8(f);
    Check(group.Passed(), L"METADATA %ls (%d cases)%ls", FromUtf8(group.name).c_str(), group.cases, failed.c_str());
}

}  // namespace

void TestMetadata(const std::wstring& media, const std::wstring& scratch) {
    for (const auto& group : metadata_checks::All(scratch)) Report(group);

    // The platform half of image validation: what the core accepts, WIC decodes; what it rejects
    // never gets that far, and WIC alone refuses a truncated PNG too.
    {
        metadata_checks::Group g{"WIC decodes accepted images and refuses broken ones"};
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto decode = [](const std::string& bytes, int& w, int& h) {
            ComPtr<IStream> s;
            s.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), (UINT)bytes.size()));
            ArtImage image;
            return s && DecodeImage(s.Get(), 64, image, 100000000, &w, &h) && !image.Empty();
        };
        int w = 0, h = 0;
        g.Add(decode(metadata_checks::Jpeg(), w, h) && w == 16 && h == 12, "the JPEG decodes at its own size");
        g.Add(decode(metadata_checks::Png(), w, h) && w == 8 && h == 8, "the PNG decodes at its own size");
        g.Add(!decode("plain text, no picture in it", w, h), "text is refused");
        Report(g);
    }

    // Every view decodes covers through LoadAlbumArt, so a custom cover shown there is shown
    // everywhere; the tag editor asks for the file's own instead.
    {
        metadata_checks::Group g{"a custom cover comes first wherever art is loaded, but not in the tag editor"};
        std::wstring dir = scratch + L"\\metadata-custom", track = dir + L"\\song.flac";
        DeleteTestFolder(dir);
        CreateDirectoryW(dir.c_str(), nullptr);
        ArtImage art;
        g.Add(!LoadAlbumArt(track, 64, art), "a track without a cover shows none");
        std::wstring error;
        customcover::Start(dir);
        g.Add(customcover::Set({track}, metadata_checks::Jpeg(), error), "a custom cover is chosen");
        g.Add(LoadAlbumArt(track, 64, art) && !art.Empty(), "the track now shows it");
        g.Add(!LoadAlbumArt(track, 64, art, false), "the tag editor still sees what the file holds");
        DeleteFileW(customcover::ImageFor(track).c_str());
        g.Add(!LoadAlbumArt(track, 64, art), "a custom image gone missing falls back to the track's own");
        customcover::Start(L"");
        DeleteTestFolder(dir);
        Report(g);
    }

    testlog::Batch batch(L"METADATA an embedded cover is one undoable edit on FLAC and MP3");
    std::wstring dir = scratch + L"\\metadata-embed", journal = dir + L"\\journal";
    DeleteTestFolder(dir);
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW(journal.c_str(), nullptr);
    std::vector<std::wstring> copies;
    bool ready = true;
    for (const wchar_t* name : {L"sine.flac", L"sine.mp3"}) {
        copies.push_back(dir + L"\\" + name);
        ready = ready && WriteTestBytes(copies.back(), ReadTestBytes(media + L"\\" + name));
    }
    if (!Ready(ready, L"embedding fixtures are copied")) return;
    // The edits come from the same path the dialogs use: one album, its tracks, one image.
    using namespace app;
    coverfetch::Album album;
    for (const auto& path : copies) album.tracks.push_back(g_lib.Add(path));
    album.dir = dir;
    size_t unsupported = 0;
    auto edits = coverfetch::EmbedEdits({{album, metadata_checks::Jpeg()}}, unsupported);
    Check(edits.size() == 2 && unsupported == 0 && edits[0].second.cover == metadata_checks::Jpeg() &&
              !edits[0].second.title && !edits[0].second.artist,
          L"METADATA embedding edits set only the cover, for every track that can hold one");
    Report(metadata_checks::EmbedUndo(copies, journal));
    DeleteTestFolder(dir);
}
