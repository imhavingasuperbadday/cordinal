// Disc art: a picture of the disc itself (a CD or a record, with transparent corners), as fanart.tv
// and many rips provide it. cordinal shows it peeking out from behind the cover.
#pragma once
#include <string>

// The disc image for a track: cd<N>.png or disc<N>.png for disc N of a set, then cd.png, disc.png or
// vinyl.png, in the track's folder, its Artwork or Scans folder, or the Artwork or Scans folder of
// the folder above (a set kept in per-disc folders). Names match without case. "" when there is none.
std::wstring FindDiscArt(const std::wstring& mediaPath, unsigned disc, unsigned discTotal);

// The cover's side when its disc shows in a square `side` pixels wide: the cover sits at the left,
// and the disc, as large, ends at the square's right edge, behind the cover. 0 when the square is
// too small for a disc to read as one.
int DiscCoverSide(int side);
