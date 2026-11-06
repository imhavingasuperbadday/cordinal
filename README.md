<p align="center"><img src="docs/icon.png" alt="cordinal icon" width="128"></p>

<h1 align="center">cordinal</h1>

<p align="center">
A small, fast music player and library for Windows, with a Linux preview.<br>
One 1.7 MB program. No installer, no runtime, no codecs to add.
</p>

<div align="center">

Current release: **0.1.7** · `cordinal.exe`, 1,697,792 bytes<br>
**[Download](https://github.com/imhavingasuperbadday/cordinal/releases/latest)** ·
[Getting started](#getting-started) · [Guide](#guide) · [Building](#building) ·
[Issues](https://github.com/imhavingasuperbadday/cordinal/issues)

</div>

cordinal plays almost any audio file with its own decoders, keeps your ratings and listening
history, and leaves your music where it is. It opens instantly, stays responsive with a large
library, and treats your files carefully: every tag write is atomic and can be undone.

- **Built in, not bolted on.** Every decoder, the equalizer, ReplayGain scanning, tag editing and
  scrobbling are part of the one executable.
- **Your files stay safe.** A failed write leaves the original byte-identical, and edits across many
  files are undoable.
- **Your library stays yours.** Music stays in your folders; tags, ratings and playlists can be
  written out in standard forms any other player reads.
- **Private by default.** cordinal never goes online unless you turn on a feature that needs to, and
  it collects nothing about you.

## Features

### Playback and sound

- Gapless playback, exclusive bit-perfect output, speed and tempo control
- Sample-rate conversion that preserves treble and suppresses aliasing
- ReplayGain with a built-in loudness scanner
- Ten-band equalizer with headphone (AutoEq) profiles, crossfeed and per-output settings
- Effects: reverb, night mode and a bitcrusher, each of which can be switched off
- Internet radio, and a browser player for listening from your phone

### Formats

FLAC, WAV, AIFF, MP3, MP4/M4A, Ogg Vorbis, Opus, AAC, ALAC, WMA, APE, WavPack, Musepack, AC3 and
E-AC-3, TTA, Matroska audio, DSD, CUE sheets, tracker modules, MIDI, video game music (NES, SNES,
Game Boy, Genesis, Commodore 64) and cordinal's own lossless `.cord`.

### Library and playlists

- Watched folders: new files join on their own, moved files keep their history, unplugged drives
  lose nothing
- Track list grouped by album, cover grid, albums with tracks, artists, and a column browser
- An optional Inbox to review new music before it joins the library
- Album editions (remaster, deluxe, regional...) sharing one tile, with an edition selector
- Static playlists, rules-based auto-playlists, a play queue and Play next
- M3U/M3U8 and PLS import and export; undo and redo for playlist changes
- Steam soundtrack import, Copy to folder or device, and classic iPod sync

### Tags and artwork

- Edit every field, the cover and lyrics for one track or hundreds, with undo
- Organize files: rename and move music from its tags, with a preview
- Covers from your files, the clipboard, or (opt-in) Deezer, iTunes and MusicBrainz
- Lyrics from tags, `.lrc` files, or (opt-in) LrcLib and lyrics.ovh, with timed lyrics preferred
- Disc art peeking out behind the cover, and artist info from MusicBrainz and Wikipedia

### Listening history

- Half-star ratings, play and skip counts, a play log and a year in review
- Native Last.fm, ListenBrainz and Libre.fm scrobbling with offline queues
- Discord status showing the song or album you're playing

### Make it yours

- Light, Dark, or follow Windows
- Window sizes from Big mode down to a mini player, with dockable panels and layout presets
- Columns, title formats, and a seek bar that can be a waveform or a moodbar
- Packs: folders or `.cordpack` files that add columns, layouts, EQ presets and themes
- Helpers: separate programs such as visualizers, running in their own windows
- A command palette (Ctrl+Shift+P) that finds any command or setting

## Getting started

1. Download `cordinal.exe` from [Releases](https://github.com/imhavingasuperbadday/cordinal/releases/latest)
   and run it.
2. Add your music folders in **Preferences ▸ Library ▸ Library folders** (Ctrl+,), or from the
   **Library** menu. Nothing is scanned until you do.
3. Double-click a track to play it. Press **Ctrl+F** to search and **Ctrl+Shift+P** to find any
   command or setting by name.
4. Everything is off by default that would go online: turn on the sources you want in
   **Preferences ▸ Online** and **Preferences ▸ Scrobbling**.

cordinal runs on Windows 10 and 11. For Linux, see [Linux](#linux-preview).

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
| Ctrl+Z / Ctrl+Y | Undo the last playlist change or tag edit / redo a playlist change |
| Ctrl+S | Save the playlist as M3U8 |
| Ctrl+N / F2 / Ctrl+W | New / rename / close playlist |
| Ctrl+E | Equalizer |
| Ctrl+L | Lyrics panel |
| Ctrl+Shift+M | Mini player |
| Ctrl+Shift+P | Command palette |
| Ctrl+, | Preferences |

## Guide

- [Library](#library)
- [Album editions](#album-editions)
- [Playlists and auto-playlists](#playlists-and-auto-playlists)
- [Sound and ReplayGain](#sound-and-replaygain)
- [Tags](#tags)
- [Covers](#covers)
- [Lyrics and artist info](#lyrics-and-artist-info)
- [Scrobbling](#scrobbling)
- [Discord status](#discord-status)
- [Devices: copying and iPod sync](#devices-copying-and-ipod-sync)
- [Seek bar, waveform and moodbar](#seek-bar-waveform-and-moodbar)
- [Layouts, packs and helpers](#layouts-packs-and-helpers)
- [Your data](#your-data)

### Library

Add folders in **Preferences ▸ Library ▸ Library folders** or from the **Library** menu. Every
subfolder is scanned, new files join on their own, moved files keep their history, and an
unplugged drive loses nothing. Scans skip empty audio placeholders; if a game or download later
fills one, the folder watcher picks it up. To review new music before it joins, send it to the
Inbox instead (**Preferences ▸ Library**).

**Library ▸ Add Steam soundtracks** finds installed soundtrack albums and in-game soundtrack
folders on Windows and Linux. The search runs in the background and can be cancelled from the
Library menu; empty placeholders and effects folders are ignored. If Steam has several audio
formats, you choose whether to keep one copy per song or a copy in each format. Duplicate files are
skipped, and existing library copies, files and history are kept.

### Album editions

Original, remaster, deluxe, regional and instrumental releases can share one album tile with an
edition selector. cordinal recognizes clear edition names and MusicBrainz release-group tags, and
keeps every edition's own files, tracklist and artwork. **Library ▸ Album editions** lets you group,
split, rename or compare editions, and choose a preferred edition and group cover.

An optional, cached and rate-limited MusicBrainz lookup can fill in missing release-group ids when
you turn it on in Preferences (**Library ▸ Album editions** on Linux); browsing and playback always
work offline. Playlists keep pointing at the exact files they named, and album headers and
`%edition%` show which edition a row belongs to.

### Playlists and auto-playlists

Playlists open as tabs. Auto-playlists are built from rules such as "rated 4 stars or more, not
played this month" or "added since 2026-01-01 and never played". Rules cover tags, path, codec,
ratings, play and skip counts, recent plays, dates added and played, length, bitrate, sample rate,
channels, BPM, file size and measured track gain, in nested ALL/ANY groups with an optional limit.

Ctrl+Z undoes changes to a playlist (adding, removing, moving, sorting, clearing) and Ctrl+Y redoes
them, for the session; Ctrl+Z takes back a tag edit instead when that came last.

### Sound and ReplayGain

The bitcrusher is in **Playback ▸ Bitcrush**; its depth, sample rate and mix are in
**Preferences ▸ Effects**. **Preferences ▸ Headphones** keeps separate EQ, crossfeed and ReplayGain
headroom for each output; remembering an output's EQ is opt-in. Supported headset hardware also
offers battery, sidetone, automatic power-off, Bluetooth do-not-disturb and onboard EQ controls,
and Apply writes only the choices you select. On Linux, hardware access needs normal read/write
access to the headset's hidraw interface.

**Preferences ▸ ReplayGain** has **Scan selection**, **Scan playlist** and **Scan library**. Scan
library remeasures every available library track, including earlier scans and failed attempts,
regardless of the current playlist or search; missing files and Inbox tracks are skipped.
**Cancel scan** appears while a manual scan runs. Results are stored in the library;
**Playback ▸ ReplayGain ▸ Write ReplayGain tags** saves them into the selected files.

### Tags

Edit every field, the cover and lyrics for one track or hundreds (Alt+Enter). Every write is
atomic, so a failure leaves the file as it was, and Ctrl+Z undoes the last ten edits. **Organize
files** renames and moves music from its tags, with a preview and undo.

### Covers

Right-click an album or some of its tracks and choose **Change cover...** (in the tag editor:
**Find cover online...**, beside **Choose cover...**). Use **From file...**, paste an image with
Ctrl+V, or drag one in from a browser or a folder; a copied image address works too. **Google
Images** opens your browser on a search for the album: copy the picture you like there, then paste
it. cordinal never contacts Google itself.

With a cover source on, the picker also shows covers from Deezer, iTunes and MusicBrainz side by
side, with their sizes. Fix the artist or album and search again, then double-click the one you
want. Each source is off until you turn it on in **Preferences ▸ Online**, where each one says what
it sends: the album's artist and title, and for MusicBrainz the release it finds to the Cover Art
Archive.

By default a chosen cover is **shown in cordinal only**, for exactly the tracks you selected, even
over a cover a file already has, and no music file changes; **Restore original cover** takes it
back. Covers can instead be saved as `cover.jpg` in the album folder (never replacing one already
there unless you ask) or embedded in the files' tags as one undoable edit.

**Library ▸ Fetch missing covers...** searches in the background for every album with no cover
(**Cancel cover search** stops it), then lists what it found: good matches checked, lower scores
unchecked. Nothing is kept until you choose Save.

An album with disc art (`cd.png`, `cd2.png` for disc 2, `disc.png` or `vinyl.png` in its folder or
its `Artwork` or `Scans` folder, as fanart.tv names them) shows the disc peeking out behind the
cover.

### Lyrics and artist info

The lyrics panel (**View ▸ Lyrics panel**, Ctrl+L) shows lyrics from the song's tags or a `.lrc`
file beside it. With a lyrics source on in **Preferences ▸ Online**, it asks LrcLib, then
lyrics.ovh, for songs that have none. Timed lyrics are preferred, only lyrics for the same title are
used, and the panel says where they came from. They are written nowhere until you right-click the
panel and choose **Save lyrics to tags** (undoable) or **Save as .lrc file** (never replacing one).

**View ▸ Artist info...** (or a track's right-click menu) shows the selected or playing track's
artist with the opening of their English Wikipedia article and buttons for MusicBrainz, Wikipedia
and the official site. It shows an artist only when exactly one MusicBrainz artist has that name.

### Scrobbling

Open **File ▸ Scrobbling...** or **Preferences ▸ Scrobbling ▸ Scrobbling...** and connect any
combination of Last.fm, ListenBrainz and Libre.fm. ListenBrainz uses your user token; Last.fm and
Libre.fm use their own application API key and secret (**Get API key** opens the service's
registration page), followed by browser approval and **Finish sign-in**. No external scrobbler is
needed.

- Credentials are protected with Windows account encryption, or an owner-only file on Linux.
- Each service keeps its own queue across restarts and retries offline listens.
- Preferences can pause all scrobbling, or turn off now-playing updates while still sending
  completed listens.
- Disconnecting stops scrobbling and removes the saved credentials. Waiting listens stay with
  their account.
- A song counts after half its duration or four minutes of actual listening; Last.fm and Libre.fm
  also require it to be longer than 30 seconds.

### Discord status

**Preferences ▸ Scrobbling ▸ Discord status** shows what you're listening to as your Discord status:
**Song** (title, artist and a progress bar) or **Album** (album, its artist and the time spent on
it). It talks only to the Discord app on the same computer, needs no account setup, and shows
nothing while paused or stopped. It is off until you choose.

Discord can only show pictures from the web. A cover you picked from the online results keeps the
address it came from, so Discord shows that same picture; for a cover from a file, the album's cover
from the sources in **Preferences ▸ Online** stands in. To show your own files' covers too, set an
upload command in `settings.ini`:

```ini
[Scrobbling]
DiscordCoverUpload=C:\Tools\upload.exe --json
```

cordinal runs that command with the cover image's path appended as one argument, keeps the first
https address it prints, and remembers it beside the cover, so the tool runs once per image. It
never runs unless you set it; a command that fails, prints nothing, or takes longer than ten
seconds costs only that image's picture.

### Devices: copying and iPod sync

**Copy to folder or device** copies music to SD cards, music players and phones.

**Library ▸ Sync to iPod...** writes the selected tracks (or the active playlist) into the master
playlist of a mounted classic iPod and keeps a copy of its previous iTunesDB. It accepts MP3, AAC,
ALAC, WAV, AIFF and audiobook files. iOS devices and protected purchases are not handled; play
protected purchases in their authorized app.

### Seek bar, waveform and moodbar

**Preferences ▸ View ▸ Seek bar** picks how the seek bar looks:

- **Progress bar**, **Waveform**, or a bar coloured by its sound. Each moment's balance of bass and
  treble turns its hue to one side of the theme's accent or the other, and louder moments stand out
  more: **Moodbar** draws a flat stripe, **Mood waveform** keeps the waveform's shape.
- **Seek bar shows** the **Whole song**, or **Live audio**, a moving picture of what is being heard
  (optionally **with progress**). Radio always shows live audio, and seeking works either way.
- **Seek bar glow** draws a soft halo that pulses with the beat, brighter where the song is loud.
- **Moodbar colours**: Theme, Vivid (the whole colour wheel), Spectrum (red bass, green mids, blue
  treble), Warm, Cool, Colour-blind friendly (blue to orange) or Single colour. **Mood waveform
  colours** can give the Mood waveform a scheme of its own.
- **Waveform colour** replaces the theme's colour with a named one (kept readable on the player);
  the moodbar's Theme, Vivid and Single colour schemes turn from it. **Waveform unplayed part**
  shows what is still to play in the theme's grey or in that colour faded.
- **Waveform reflection** is **Mirrored** below the centre line, **Upper half**, or **Full height**
  (mirrored as bright below the line as above; the Moodbar stripe keeps the upper half).
- **Smooth seek bar** rounds the outline and blends the colours.

Over the whole song, both moodbar looks draw a playhead line in the playback state's colour: amber
while paused, red when playback will stop or the track failed. The track list's **Mood** column
shows each track's moodbar; tracks not seen before fill in while nothing else is being prepared, and
a ReplayGain scan prepares them all.

### Layouts, packs and helpers

**View ▸ Window size** offers Compact, Big mode plus and Big mode, down to the mini player
(Ctrl+Shift+M). Big mode has draggable panel tabs and splitters: F6 focuses a group, arrows select a
tab, Delete closes it, Ctrl+Up/Down moves it between the two areas, Ctrl+Shift+arrow splits it, and
Alt+arrow resizes its split. Reopen panels from **View ▸ Big mode panels**. **Startup defaults**
lets you choose which window size and layout to keep for the next start.

**Packs** bundle customization as a folder or a zipped `.cordpack` file; install and enable them
from **View ▸ Packs**. Enabled packs add columns and named column sets to the track list's header
menu, EQ presets to the equalizer, themes, auto-playlists, shortcuts and layout presets to **View ▸
Customize layout ▸ Presets**. Layouts include docking on every edge, large covers, Up next, panel
placement and waveform options. Turning off a pack removes what it supplied.

On Linux, a pack's global shortcuts use the desktop's Global Shortcuts portal, which may ask you to
approve or adjust the keys; a missing portal or a declined request is reported in the pack's
warnings. Linux themes can use installed fonts or TTF/OTF files in an enabled pack's `fonts/`
folder: set the theme's `font` to the family name. Pack fonts stay private to cordinal and are
released when the pack is turned off; menus keep the system font.

**Helpers** are separate programs cordinal can run. **View ▸ Helpers ▸ Visualizers** runs enabled
helper visualizers in their own windows. Each gets a read-only audio feed and stops when its helper
is turned off or cordinal closes.

**Preferences ▸ View ▸ Draw with the graphics card** switches drawing immediately.

### Your data

| | Windows | Linux |
|---|---|---|
| Library, playlists, statistics | `%APPDATA%\cordinal` | `$XDG_DATA_HOME/cordinal` |
| Settings | `%APPDATA%\cordinal` | `$XDG_CONFIG_HOME/cordinal` |
| Cover and waveform caches | `%LOCALAPPDATA%\cordinal` | `$XDG_CACHE_HOME/cordinal` |

cordinal only goes online when you enable a feature that needs it: Radio, scrobbling, Discord
status covers, the browser player, the album-edition lookup, or a cover, lyrics or artist info
source in **Preferences ▸ Online**.

**Back up.** **File ▸ Back up library and settings** saves everything in a folder you can read,
copy or zip. **File ▸ Restore backup** brings it back, on the same computer or a new one where the
music lives somewhere else.

**Export your collection.** **File ▸ Export collection...** saves the music itself in one `.zip`:
**Export entire collection** for every song the Library tab shows or a playlist holds, or **Select
what to export** to tick artists, albums, songs and playlists (a ticked playlist brings all its
songs). The music is arranged as in its library folder (songs from elsewhere go under `Other`), with
the folders' cover images, cue sheets and lyrics, and the playlists as `.m3u8` files in
`Playlists`. Your files are only read, and an export that fails or is cancelled leaves no archive.
Any unzip program opens it. A FAT32 drive can't hold an archive of 4 GB or more.

**File ▸ Import collection...** takes archives cordinal exported. It unpacks one into a folder you
choose, checks every file against the archive, adds the music to your library folders and opens the
playlists as tabs. It never replaces a file: an identical one is left as it is, and a different one
with the same name stays while the collection's copy goes beside it, so importing the same archive
again only fills in what is missing. Ratings and play counts aren't in the archive; a backup
carries those.

**Leaving cordinal.** Nothing you do in cordinal is locked inside it:

- **Tags** are written into the music files, in the standard fields other players read.
- **Ratings and play counts**: select tracks (Ctrl+A in the Library tab for all of them), then
  right-click ▸ **Rating ▸ Write ratings and play counts to files**. They go in as `FMPS_RATING`
  and `FMPS_PLAYCOUNT` (MP3s also get a `POPM` rating) in MP3, FLAC, Ogg, Opus, M4A and `.cord`
  files. Tracks without a rating or plays keep whatever their files had, and Ctrl+Z takes it back.
- **Playlists**: Ctrl+S saves the open one as `.m3u8`, which any player opens.
- **Everything at once**: a backup folder holds `statistics.csv`, `history.csv` and every playlist
  as `.m3u8`, readable without cordinal.

## Linux (preview)

A native Linux build shares the library, decoders, tags and engine with Windows. Its window is drawn
through SDL3, sound goes through PipeWire, and media keys and the tray use D-Bus. It has the same
features as Windows but is less tested than the Windows build.

On Arch Linux, one command installs it and the libraries it needs:

```sh
curl -fsSL https://github.com/imhavingasuperbadday/cordinal/releases/latest/download/install.sh | sh
```

Run it again to update. Replace the final `sh` with `sh -s -- --uninstall` to remove it; your
library and settings stay. On other distributions, use `cordinal-linux-x64.tar.gz` from
[Releases](https://github.com/imhavingasuperbadday/cordinal/releases/latest) with the libraries
listed below, or build from source.

## Building

**Windows** needs Visual Studio 2022 (or its Build Tools) with the C++ x64 workload:

```bat
build.bat unpacked
```

That makes a quick development build. Plain `build.bat` also packs the program, downloading the
pinned UPX packer and checking its hash. Releases use cordinal's own packer: `build.bat cdpack`
additionally needs [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)'s `clang++` on `PATH` and
Python 3.14 (see [cdpack/README.md](cdpack/README.md)). Every mode writes `build\cordinal.exe`.

**Linux** needs CMake, a C++20 compiler, SDL3, SDL3_ttf, PipeWire, systemd-libs, fontconfig and
OpenSSL:

```sh
cmake -S . -B build-linux && cmake --build build-linux
```

## Contributing and support

- **Bugs and format requests:** open an [issue](https://github.com/imhavingasuperbadday/cordinal/issues/new/choose)
  with your cordinal version and the steps. Attach a crash report from `%LOCALAPPDATA%\cordinal\crashes`
  if there is one.
- **Security problems:** report them privately, as [SECURITY.md](SECURITY.md) describes.
- **Code:** read [CONTRIBUTING.md](CONTRIBUTING.md) first. Ideas for new features are best opened as
  an issue before any work starts.

## License

cordinal is free software under the GNU GPL, version 3 or later ([LICENSE](LICENSE)). It is an
original implementation; [THIRD_PARTY.md](THIRD_PARTY.md) lists the outside data and code it
carries.
