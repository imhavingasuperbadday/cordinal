// Albums, missing covers, the batch search and saving chosen covers (coverfetch.h). Folder covers
// go through metadata_fetch's atomic write and custom ones through customcover's; embedded ones are
// TagEdit::cover edits the front end hands to its tag writer, so one batch is one undoable edit and
// a failed write leaves the file as it was.
#include "coverfetch.h"

#include "customcover.h"
#include "tags.h"

#include <windows.h>

#include <algorithm>
#include <map>

namespace coverfetch {

namespace {

std::wstring RealFile(const std::wstring& path) {
    std::wstring file;
    int index = 0;
    return SplitSubtrack(path, file, index) ? file : path;
}

bool IsFolder(const std::wstring& dir) {
    DWORD attributes = GetFileAttributesW(dir.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<Album> Collect(const std::vector<TrackId>& ids, bool needAlbumTag) {
    std::vector<Album> albums;
    std::map<std::pair<StringPool::Handle, std::string>, size_t> index;
    for (TrackId id : ids) {
        if (!g_lib.Contains(id)) continue;
        const Track& t = g_lib[id];
        std::string album = ToUtf8(g_lib.Str(t.album));
        if (album.empty() && needAlbumTag) continue;
        std::string lowered = album;
        for (char& c : lowered) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        auto [it, added] = index.try_emplace({t.dir, lowered}, albums.size());
        if (added) {
            Album a;
            a.firstPath = RealFile(g_lib.Path(id));
            a.dir = DirectoryOf(a.firstPath);
            a.album = album.empty() ? ToUtf8(FileNameOf(a.dir)) : album;
            a.artist = ToUtf8(g_lib.Str(t.albumArtist ? t.albumArtist : t.artist));
            if (a.artist.empty() && (t.flags & Track::kCompilation)) a.artist = "Various Artists";
            albums.push_back(std::move(a));
        }
        albums[it->second].tracks.push_back(id);
        albums[it->second].paths.push_back(g_lib.Path(id));
    }
    return albums;
}

}  // namespace

std::wstring Album::Label() const {
    std::wstring a = FromUtf8(artist), b = FromUtf8(album);
    return a.empty() ? b : b.empty() ? a : a + L" \x2013 " + b;
}

std::vector<Album> AlbumsOf(const std::vector<TrackId>& ids) { return Collect(ids, false); }

std::vector<Album> LibraryAlbums() {
    std::vector<TrackId> ids;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if (g_lib[id].InLibrary()) ids.push_back(id);
    return Collect(ids, true);
}

bool AlbumHasCover(const Album& album) {
    if (customcover::Count(album.paths) || online::FolderHasCover(album.dir)) return true;
    std::string image;
    return ReadEmbeddedArt(album.firstPath, image) && !image.empty();
}

void RunBatch(const std::vector<Album>& albums, online::Cancel& cancel, const online::ImageDecoder& decode,
              int thumbSize, const std::function<void(size_t, size_t)>& progress, BatchResult& out) {
    out = {};
    out.albums = albums.size();
    const auto sources = online::CoverOrder();
    for (size_t i = 0; i < albums.size(); ++i) {
        if (cancel.Stopped()) break;
        if (out.proposals.size() >= kMaxProposals) {
            out.limited = true;
            break;
        }
        if (progress) progress(i, albums.size());
        const Album& album = albums[i];
        if (!IsFolder(album.dir) || AlbumHasCover(album)) {
            ++out.hadCover;
            continue;
        }
        online::CoverSearch search;
        search.query = {album.artist, album.album};
        search.sources = sources;
        search.thumbSize = thumbSize;
        bool found = online::SearchCovers(search, cancel, decode, [&](online::CoverFound&& cover) {
            Proposal p;
            p.album = album;
            p.confident = online::ConfidentCover(search.query, cover.candidate);
            p.cover = std::move(cover);
            out.proposals.push_back(std::move(p));
        });
        if (!found && !cancel.Stopped()) ++out.notFound;
    }
    out.cancelled = cancel.Stopped();
    std::stable_sort(out.proposals.begin(), out.proposals.end(), [](const Proposal& a, const Proposal& b) {
        if (a.confident != b.confident) return a.confident;
        return a.cover.candidate.score > b.cover.candidate.score;
    });
}

std::vector<CoverChoice> CheckedChoices(const std::vector<Proposal>& proposals, const std::vector<bool>& checked) {
    std::vector<CoverChoice> choices;
    for (size_t i = 0; i < proposals.size() && i < checked.size(); ++i)
        if (checked[i]) {
            // A candidate's address is where its image came from: a provider's result, or a link the user pasted.
            // A picture handed over from a file has none, and its cover then falls back to the album's.
            const online::CoverFound& cover = proposals[i].cover;
            choices.push_back({proposals[i].album, cover.image, cover.candidate.url});
        }
    return choices;
}

FolderResult SaveToFolders(const std::vector<CoverChoice>& choices, bool overwrite) {
    FolderResult result;
    for (const CoverChoice& choice : choices) {
        std::wstring error;
        switch (online::WriteFolderCover(choice.album.dir, choice.image, overwrite, error)) {
            case online::FolderWrite::Written:
                ++result.written;
                result.changed.insert(result.changed.end(), choice.album.tracks.begin(), choice.album.tracks.end());
                break;
            case online::FolderWrite::Exists: ++result.kept; break;
            case online::FolderWrite::Failed:
                ++result.failed;
                result.errors.push_back(choice.album.Label() + L": " + error);
                break;
        }
    }
    return result;
}

FolderResult SaveCustom(const std::vector<CoverChoice>& choices) {
    FolderResult result;
    result.custom = true;
    for (const CoverChoice& choice : choices) {
        std::wstring error;
        if (customcover::Set(choice.album.paths, choice.image, error, choice.url)) {
            ++result.written;
            result.changed.insert(result.changed.end(), choice.album.tracks.begin(), choice.album.tracks.end());
        } else {
            ++result.failed;
            result.errors.push_back(choice.album.Label() + L": " + error);
        }
    }
    return result;
}

std::vector<std::pair<TrackId, TagEdit>> EmbedEdits(const std::vector<CoverChoice>& choices, size_t& unsupported) {
    std::vector<std::pair<TrackId, TagEdit>> edits;
    unsupported = 0;
    for (const CoverChoice& choice : choices)
        for (TrackId id : choice.album.tracks) {
            if (!g_lib.Contains(id) || !CanWriteTags(g_lib.Path(id))) {
                ++unsupported;
                continue;
            }
            TagEdit edit;
            edit.cover = choice.image;
            edits.emplace_back(id, std::move(edit));
        }
    return edits;
}

}  // namespace coverfetch
