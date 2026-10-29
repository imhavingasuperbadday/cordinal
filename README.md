# cordinal

![icon](docs/icon.png)

A small, fast music player and library for Windows 10/11, with a Linux preview. It is one native
program of about 1.5 MB: no installer, no runtime, nothing else to install. It plays almost any
audio file with its own decoders, keeps your ratings and listening history, and leaves your music
where it is.

Version **0.1.0**, the first public release. Download `cordinal.exe` from
[Releases](https://github.com/imhavingasuperbadday/cordinal/releases/latest) and run it.

## What it does

- **A library that watches your folders.** Add your music folders once (Library ▸ Library
  folders); new files join on their own, moved files keep their history, and an unplugged drive
  loses nothing. New music can wait in an Inbox for review instead (Preferences ▸ Library).
- **Several ways to look at it.** A track list grouped by album, a cover grid, albums with their
  tracks, and artists, with a column browser (genre, artist, album, folder...) above them.
- **Playlists and auto-playlists.** Tabs for static playlists and for rules-based ones ("rated 4
  stars or more, not played this month"). A play queue, Play next, and M3U/M3U8 import and export.
- **Almost any format, built in.** FLAC, WAV, AIFF, MP3, Ogg Vorbis, Opus, AAC, ALAC, WMA, APE,
  WavPack, AC3, Matroska audio, DSD, CUE sheets, tracker modules, MIDI and video game music (NES,
  SNES, Game Boy, Genesis, Commodore 64), plus cordinal's own lossless `.cord`. No codecs to install.
- **Sound.** Gapless playback, ReplayGain with a built-in loudness scanner, a ten-band equalizer
  with headphone (AutoEq) profiles and crossfeed, exclusive bit-perfect output, speed and tempo,
  reverb and a night mode.
- **Tags, done carefully.** Edit every field, the cover and lyrics for one track or hundreds. Every
  write is atomic, so a failure leaves the file as it was, and Ctrl+Z undoes the last ten edits.
  Organize files renames and moves music from its tags, with a preview and undo.
- **Listening history.** Half-star ratings, play and skip counts, a play log, a year in review,
  ListenBrainz scrobbling, and points for listening if you like that sort of thing.
- **Beyond the library.** Internet radio, a browser player for your phone, and Copy to folder or
  device for SD cards, music players and phones.
- **Make it yours.** Light, dark or follow Windows; window sizes from a big player down to a mini
  player; columns and title formats; a command palette (Ctrl+Shift+P) that finds any command or
  setting. Every setting lives in Preferences (Ctrl+,).

## Your data

Everything cordinal keeps is in `%APPDATA%\cordinal` (`$XDG_DATA_HOME/cordinal` on Linux): the
library, playlists, statistics and settings. Cover and waveform caches are in
`%LOCALAPPDATA%\cordinal`. cordinal only goes online when you use Radio, ListenBrainz or the
browser player.

**File ▸ Back up library and settings** saves everything in a folder you can read, copy or zip;
**File ▸ Restore backup** brings it back, on the same computer or a new one where the music lives
somewhere else.

### Leaving cordinal

Nothing you do in cordinal is locked inside it:

- **Tags** you edit are written into the music files, in the standard fields other players read.
- **Ratings and play counts**: select tracks (Ctrl+A in the Library tab for all of them),
  right-click ▸ **Rating ▸ Write ratings and play counts to files**. They go in as `FMPS_RATING`
  and `FMPS_PLAYCOUNT` (MP3s also get a `POPM` rating) in MP3, FLAC, Ogg, Opus, M4A and .cord
  files. Tracks without a rating or plays keep whatever their files had, and Ctrl+Z takes it back.
- **Playlists**: Ctrl+S saves the open one as `.m3u8`, which any player opens.
- **Everything at once**: a backup folder holds `statistics.csv`, `history.csv` and every
  playlist as `.m3u8`, readable without cordinal.

## Keyboard

| Key | Action |
|---|---|
| Space | Play / pause |
| Enter / double-click | Play |
| Left / Right | Seek 5 seconds |
| Ctrl+Left / Ctrl+Right | Previous / next |
| Ctrl+Enter / Ctrl+Q | Play next / add to queue |
| Ctrl+F | Search |
| Alt+Enter | Edit tags |
| Ctrl+Z | Undo the last tag edit |
| Ctrl+S | Save the playlist as M3U8 |
| Ctrl+N / F2 / Ctrl+W | New / rename / close playlist |
| Ctrl+E | Equalizer |
| Ctrl+L | Lyrics panel |
| Ctrl+Shift+M | Mini player |
| Ctrl+Shift+P | Command palette |
| Ctrl+, | Preferences |

## Linux (preview)

A native Linux build shares the library, decoders, tags and engine, with its window drawn through
SDL3, sound through PipeWire, and media keys and the tray through D-Bus. It is less tested than
the Windows build. On Arch Linux, one command installs it and the libraries it needs:

```sh
curl -fsSL https://github.com/imhavingasuperbadday/cordinal/releases/latest/download/install.sh | sh
```

Run it again to update; `sh -s -- --uninstall` in place of `sh` removes it (your library stays).

## Building

Windows needs Visual Studio 2022 (or its Build Tools) with the C++ x64 workload:

```bat
build.bat
```

The first build downloads the pinned UPX packer and checks its hash. `build.bat unpacked` skips
packing for a quicker development build. Both write `build\cordinal.exe`.

Linux needs CMake, a C++20 compiler, SDL3, SDL3_ttf, PipeWire, systemd-libs, fontconfig and OpenSSL:

```sh
cmake -S . -B build-linux && cmake --build build-linux
```

## License

cordinal is free software under the GNU GPL, version 3 or later ([LICENSE](LICENSE)). It is an
original implementation; [THIRD_PARTY.md](THIRD_PARTY.md) lists the outside data and code it carries.
