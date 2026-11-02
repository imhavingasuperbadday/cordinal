# cordinal

![icon](docs/icon.png)

A small, fast music player and library for Windows 10/11, with a Linux preview. It is one native
program of about 1.5 MB: no installer, no runtime, nothing else to install. It plays almost any
audio file with its own decoders, keeps your ratings and listening history, and leaves your music
where it is.

Current release: **0.1.3**. Download `cordinal.exe` from
[Releases](https://github.com/imhavingasuperbadday/cordinal/releases/latest) and run it.

## What it does

- **A library that watches your folders.** Add your music folders in Preferences ▸ Library ▸
  Library folders, or from the Library menu. Every subfolder is scanned, new files join on their
  own, moved files keep their history, and an unplugged drive loses nothing. Nothing is scanned
  automatically until you add a folder. Scans skip empty audio placeholders; if a game or download later
  fills one, the folder watcher picks it up. New music can wait in an Inbox for review instead
  (Preferences ▸ Library).
  Library ▸ Add Steam soundtracks finds installed albums and in-game soundtrack folders on Windows
  and Linux. The search runs in the background and can be cancelled from the Library menu; empty
  audio placeholders and effects folders are ignored.
  If Steam has several audio formats, you choose whether to keep one copy per song or a copy in
  each format. Duplicate files are skipped; existing Library copies, files and history are kept.
- **Several ways to look at it.** A track list grouped by album, a cover grid, albums with their
  tracks, and artists, with a column browser (genre, artist, album, folder...) above them.
- **One album, many editions.** Original, remaster, deluxe, regional and instrumental releases can
  share one album tile with an edition selector. cordinal recognizes clear edition names and
  MusicBrainz release-group tags, keeps every edition's own files, tracklist and artwork, and lets
  you group, split, rename, compare or choose a preferred edition and group cover by hand from
  **Library > Album editions**. An optional, cached and rate-limited MusicBrainz lookup can fill in
  missing release-group ids when you turn it on in Preferences (Library > Album editions on Linux);
  browsing and playback always work offline. Existing playlists keep pointing at the exact files they named, and album headers and
  `%edition%` show which edition a row belongs to.
- **Playlists and auto-playlists.** Tabs for static playlists and for rules-based ones ("rated 4
  stars or more, not played this month", "added since 2026-01-01 and never played"). Rules cover
  tags, path, codec, ratings, play and skip counts, recent plays, dates added and played, length,
  bitrate, sample rate, channels, BPM, file size and measured track gain, in nested ALL/ANY groups
  with an optional limit. A play queue, Play next, and M3U/M3U8 import and export.
- **Almost any format, built in.** FLAC, WAV, AIFF, MP3, MP4/M4A, Ogg Vorbis, Opus, AAC, ALAC, WMA, APE,
  WavPack, AC3, Matroska audio, DSD, CUE sheets, tracker modules, MIDI and video game music (NES,
  SNES, Game Boy, Genesis, Commodore 64), plus cordinal's own lossless `.cord`. No codecs to install.
- **Sound.** Gapless playback, ReplayGain with a built-in loudness scanner, a ten-band equalizer
  with headphone (AutoEq) profiles and crossfeed, exclusive bit-perfect output, speed and tempo,
  reverb and a night mode. Sample-rate conversion preserves treble and suppresses aliasing when
  a track and the output device use different rates.
  **Preferences ▸ ReplayGain** has **Scan selection**, **Scan playlist** and **Scan library**.
  Scan library remeasures every available library track, including earlier scans and failed attempts,
  regardless of the current playlist or search. Missing files and Inbox tracks are skipped.
  **Cancel scan** appears while a manual scan runs. Scanning stores results in the library;
  **Playback ▸ ReplayGain ▸ Write ReplayGain tags** saves them into the selected files.
- **Tags, done carefully.** Edit every field, the cover and lyrics for one track or hundreds. Every
  write is atomic, so a failure leaves the file as it was, and Ctrl+Z undoes the last ten edits.
  Organize files renames and moves music from its tags, with a preview and undo.
- **Any cover you like, from your files or the internet.** Right-click an album or some of its
  tracks and choose **Change cover...** (also in the tag editor as Find cover online..., beside
  Choose cover...). Use **From file...**, paste an image with Ctrl+V or drag one in from a browser
  or a folder; a copied image address works too. **Google Images** opens your browser on a search
  for the album; copy the picture you like there, then paste it. With a cover source on, the picker
  also shows covers from Deezer, iTunes and MusicBrainz side by side, with their sizes; fix the
  artist or album and search again, then double-click the one you want. By default a chosen cover
  is **shown in cordinal only**, for exactly the tracks you selected, even over a cover a file
  already has, and no music file changes; **Restore original cover** in the same menu takes it
  back. **Library ▸ Fetch missing covers...** looks for every album that has no cover at all, in
  the background (Cancel cover search stops it), then lists what it found: good matches checked,
  lower scores unchecked. Nothing is kept until you choose Save. Covers can instead be saved as
  cover.jpg in the album folder (touching no audio files, and never replacing a cover that is
  already there unless you ask) or embedded in the files' tags as one undoable edit. Each online
  source is off until you turn it on in **Preferences ▸ Online**, where every one says what it
  sends: the album's artist and title, and for MusicBrainz the release it finds to the Cover Art
  Archive. cordinal never contacts Google itself.
- **Lyrics for songs that have none.** With a lyrics source on in **Preferences ▸ Online**, the
  lyrics panel (View ▸ Lyrics panel) asks LrcLib, then lyrics.ovh, when a song has no lyrics in its
  tags and no .lrc file beside it. Timed lyrics are preferred, only lyrics for the same title are
  used, and the panel says where they came from. They are written nowhere until you right-click the
  panel and choose **Save lyrics to tags** (undoable) or **Save as .lrc file** (never replacing one).
  **View ▸ Artist info...** (or a track's right-click menu) shows the selected or playing track's
  artist with the opening of their English Wikipedia article and buttons for MusicBrainz, Wikipedia
  and the official site. It shows an artist only when exactly one MusicBrainz artist has that name.
- **Listening history.** Half-star ratings, play and skip counts, a play log, a year in review,
  native Last.fm, ListenBrainz and Libre.fm scrobbling, and points for listening if you like that sort of thing.

  Open **File ▸ Scrobbling...** in the top menu, or **Preferences ▸ Scrobbling ▸ Scrobbling...**.
  Connect any combination of the three services. ListenBrainz uses your user token; Last.fm and
  Libre.fm use their own application API key and secret (the **Get API key** button opens the
  service's registration page), followed by browser approval and **Finish sign-in**. No helper or
  external scrobbler is needed. Credentials are protected with Windows account encryption, or an
  owner-only file on Linux. Each service keeps its own queue across restarts and retries offline
  listens. Preferences can pause all scrobbling or disable now-playing updates while still sending
  completed listens. Scrobbling is off until you connect a service; disconnecting stops it and
  removes its saved credentials. Waiting listens stay with their account.
  A song counts after half its duration or four minutes of actual listening; Last.fm and Libre.fm
  also require it to be longer than 30 seconds.
- **Beyond the library.** Internet radio, a browser player for your phone, Copy to folder or
  device for SD cards, music players and phones, and **Library ▸ Sync to iPod...** for classic
  iPods. The iPod sync writes the selected tracks (or the active playlist) into the master
  playlist of a mounted iPod and keeps a copy of its previous iTunesDB. It accepts MP3, AAC,
  ALAC, WAV, AIFF and audiobook files. iOS devices and protected purchases are not handled;
  play protected purchases in their authorized app.
- **Make it yours.** Light, dark or follow Windows; window sizes from a big player down to a mini
  player; columns and title formats; a command palette (Ctrl+Shift+P) that finds any command or
  setting. Every setting lives in Preferences (Ctrl+,).
  **Preferences ▸ View ▸ Waveform playback bar** switches between a waveform and a regular progress
  bar. Turn it off to use the regular bar even when live waveform is enabled; the live setting is kept.

## Your data

Everything cordinal keeps is in `%APPDATA%\cordinal` (`$XDG_DATA_HOME/cordinal` on Linux): the
library, playlists, statistics and settings. Cover and waveform caches are in
`%LOCALAPPDATA%\cordinal`. cordinal only goes online when you enable online features such as Radio, scrobbling, the
browser player, the album-edition lookup, or a cover, lyrics or artist info source in Preferences ▸ Online.

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

Packs bundle customization as a folder or a zipped .cordpack file. Install and enable them from
**View ▸ Packs**. Enabled packs add columns and named column sets to the track list's header menu,
EQ presets to the equalizer, and layout presets to **View ▸ Customize layout ▸ Presets**.
Layouts include docking on every edge, large covers, Up next, panel placement and waveform options.
Turning off a pack removes the columns and presets it supplies.

Pack global shortcuts use the desktop's Global Shortcuts portal, which may ask you to approve or
adjust the keys. They work while another window has focus. A desktop without this portal, or a
declined request, is reported in the pack's warnings; window shortcuts work independently.

Linux also supports Compact, Big mode plus and Big mode from **View ▸ Window size**.
Big mode has draggable panel tabs and splitters. F6 focuses a group; arrows select a tab,
Delete closes it, Ctrl+Up/Down moves it between the two areas, Ctrl+Shift+arrow splits it,
and Alt+arrow resizes its split. Reopen panels from **View ▸ Big mode panels**.
**Startup defaults** lets you choose which window size and layout to keep for the next start.

**Preferences ▸ Headphones** provides separate EQ, crossfeed and ReplayGain headroom for each
output. Remembering an output's EQ is opt-in. Supported headset hardware also offers its battery,
sidetone, automatic power-off, Bluetooth do-not-disturb and onboard EQ controls; Apply writes only
the choices you select. Hardware access needs the user's normal read/write access to that
headset's hidraw interface; the page reports unavailable controls and access failures.

**View ▸ Helpers ▸ Visualizers** runs enabled helper visualizers in their own windows. Each gets
a read-only shared audio feed, and stops when its helper is turned off or cordinal closes.
**Preferences ▸ View ▸ Draw with the graphics card** switches immediately.

Linux themes can use installed fonts or TTF/OTF files in an enabled pack's `fonts/` folder
(View ▸ Packs). Set the theme's `font` to the font's family name. Pack fonts take effect immediately,
stay private to cordinal and are released when the pack is turned off; menus keep the system font.

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
