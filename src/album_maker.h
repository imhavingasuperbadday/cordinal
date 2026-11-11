// Making an album from tracks (Edit > Make album...), shared by the Windows and Linux album makers:
// the values a draft starts with, its track order, and the tag edits that make it an album.
//
// An album here is made of tags (album, album artist, year, genre, numbers, cover), so it shows
// wherever an album does: the album views and headers, Now Playing, an iPod and other players. The
// write goes through the ordinary tag writer, so Edit > Undo last tag edit takes it back.
#pragma once
#include "library.h"
#include "tagwriter.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

struct AlbumDraft {
    std::vector<TrackId> tracks;  // in album order; each once
    std::wstring title, artist;   // the album and its album artist (required)
    std::wstring date, genre;     // empty leaves each track's own
    bool compilation = false;     // various artists: marks the tracks, and names "Various Artists" when artist is empty
    bool numberTracks = true;     // track numbers follow the order, with the total
    bool artistToTracks = false;  // each track's artist becomes the album artist
    uint32_t disc = 0, discTotal = 0;  // 0 leaves each track's own
    std::optional<std::string> cover;  // JPEG or PNG bytes for every track; "" removes; nullopt leaves them
};

enum class AlbumOrder { Numbers, Title, FileName, Reverse };

// A draft for `ids` (duplicates and unknown ids dropped): each field holds the value most tracks
// share; the album artist falls back to a shared artist, or to Various Artists when the artists
// differ. Tracks that all carry numbers keep that order and their numbers; otherwise the order is as
// given and Number the tracks is on.
AlbumDraft DraftAlbum(const Library& library, const std::vector<TrackId>& ids);
void OrderAlbum(const Library& library, AlbumDraft& draft, AlbumOrder order);
// Moves the tracks at `positions` (sorted, distinct) one place up (-1) or down (1) as a block, and
// returns their new positions; at an end nothing moves.
std::vector<size_t> MoveAlbumTracks(AlbumDraft& draft, const std::vector<size_t>& positions, int delta);
// Adds the tracks that are not in the draft yet, at the end. Returns how many were added.
size_t AddAlbumTracks(const Library& library, AlbumDraft& draft, const std::vector<TrackId>& ids);
// What keeps the draft from being saved, as one sentence; empty when it can be.
std::wstring AlbumDraftProblem(const AlbumDraft& draft);
// One edit per track that changes something, holding only the fields that differ from its tags now.
std::vector<std::pair<TrackId, TagEdit>> AlbumEdits(const Library& library, const AlbumDraft& draft);
