<p align="center"><img src="docs/icon.png" alt="cordinal icon" width="128"></p>

<h1 align="center">cordinal</h1>

<p align="center">
A small, fast music player for Windows, with a Linux preview.<br>
One program, under 2 MB. No installer, no runtime, no codecs to add.
</p>

<div align="center">

Current release: **0.1.15** · `cordinal.exe`, 1,951,744 bytes<br>
**[Download](https://github.com/imhavingasuperbadday/cordinal/releases/latest)** ·
[Getting started](#getting-started) · [Guide](#guide) · [Building](#building) ·
[Issues](https://github.com/imhavingasuperbadday/cordinal/issues)

</div>

**cordinal** is a free music player and library for Windows. It plays almost any audio file with
its own built-in decoders, remembers your ratings and listening history, and leaves your music in the
folders where you keep it. The whole player is a single file: download it and run it. A native Linux
version is available as a preview.

cordinal is written in C++ and is free software under the GNU GPL.

## Why cordinal

- **Everything is built in.** Decoders, the equalizer, loudness scanning, tag editing and
  scrobbling are all part of the one program.
- **Your files are safe.** If saving a tag fails, the file stays exactly as it was. Edits to many
  files at once can be undone.
- **Tested against broken files.** Every decoder, the tag reader and the cover-art reader are
  fuzzed under AddressSanitizer and UndefinedBehaviorSanitizer in CI on every push, and the Linux
  core self-tests run under both sanitizers too. The Opus decoder passes the official conformance
  vectors. A release is published only when all of this passes, and anyone can read the CI logs.
- **Your music stays yours.** Files stay in your own folders, and tags, ratings and playlists can be
  saved in standard forms that other players read.
- **Private by default.** cordinal does not go online until you turn on a feature that needs to. It
  collects nothing about you.

## Features

**Playback.** Gapless playback or crossfade. Bit-perfect Exclusive and ASIO output, and native DSD
to a DAC that takes it. Speed and tempo control. ReplayGain with a built-in loudness scanner. A
ten-band equalizer with headphone (AutoEq) profiles and crossfeed. Reverb, night mode and a
bitcrusher. Stereo upmixed to 5.1 or 7.1. Internet radio, a browser player for your phone, and
network (UPnP/DLNA) speakers.

**Formats.** FLAC (also in Ogg), WAV, AIFF, MP3, MP4/M4A, Ogg Vorbis, Opus, AAC, ALAC, WMA, APE,
WavPack, Musepack, AC3 and E-AC-3, TTA, Matroska audio, DSD, Apple CAF, Sun AU, Creative VOC, Amiga
8SVX, CRI ADX, GSM 06.10, G.722, G.726, ComputerCraft DFPWM, CUE sheets, tracker modules, MIDI, video
game music (NES, SNES, Game Boy, Genesis, Commodore 64) and cordinal's own lossless `.cord`.

**Library.** Watched folders that keep up with new and moved files. Track list, cover grid, albums,
artists and a column browser. Album editions (remaster, deluxe and so on) under one cover.
Playlists, rule-based auto-playlists and a drag-to-reorder queue. M3U, M3U8 and PLS. Steam
soundtrack import, copying to devices, and classic iPod sync.

**Tags and artwork.** Edit tags, covers and lyrics for one track or hundreds, with undo. Rename and
move files from their tags. Convert to FLAC, WAV, MP3, AAC or `.cord`, as copies or as undoable
replacements. Find duplicates and missing tags. Covers, lyrics and artist info from your files or,
if you turn them on, from online sources.

**Listening history.** Half-star ratings, play and skip counts, a play log and a year in review.
Shuffle that can learn your taste. Last.fm, ListenBrainz and Libre.fm scrobbling. Discord status.

**Look and feel.** Light and Dark themes, tinted by the playing album's cover if you like. Window
sizes from Big mode down to a mini player. Lightweight mode for playing in the background. A
waveform or moodbar seek bar. Packs and helpers for customization. A command palette
(Ctrl+Shift+P) that finds any command or setting.

## Getting started

1. Download `cordinal.exe` from
   [Releases](https://github.com/imhavingasuperbadday/cordinal/releases/latest) and run it.
2. Add your music folders in **Preferences ▸ Library ▸ Library folders** (Ctrl+,), or from the
   **Library** menu. Nothing is scanned until you do this.
3. Double-click a track to play it. Press **Ctrl+F** to search, and **Ctrl+Shift+P** to find any
   command or setting by name.
4. Online features are off until you turn them on in **Preferences ▸ Online** and
   **Preferences ▸ Scrobbling**.

To update, replace `cordinal.exe` with the new download. Your library, playlists, ratings and
settings are kept. To move them to another computer, use **File ▸ Back up library and settings**.

cordinal runs on 64-bit Windows 10 and 11. For Linux, see [Linux](#linux-preview).

**Preferences** (Ctrl+,) are grouped under Sound, Look, Collection and General. Every setting has
its explanation beside it, and a **Reset** button appears when it differs from the default. **All
settings** shows every page at once, and the search box looks through all of them.

## Keyboard

| Key | Action |
|---|---|
| Space | Play / pause |
| Enter / double-click | Play |
| Left / Right | Seek 5 seconds |
| Ctrl+Left / Ctrl+Right | Previous / next |
| Ctrl+Enter / Ctrl+Q | Play next / add to queue |
| Ctrl+Shift+Q | Up next (the queue) |
| Ctrl+F | Search |
| Alt+Enter | Edit tags |
| Ctrl+Z / Ctrl+Y | Undo the last playlist change or tag edit / redo a playlist change |
| Ctrl+S | Save the playlist as M3U8 |
| Ctrl+N / F2 / Ctrl+W | New / rename / close playlist |
| Ctrl+E | Equalizer |
| Ctrl+L | Lyrics panel |
| Ctrl+Shift+M | Mini player |
| Ctrl+Shift+L | Lightweight mode |
| Win+Alt+Space | In Lightweight mode, from any program: pick a song |
| Ctrl+Shift+P | Command palette |
| Ctrl+, | Preferences |

You can change any key in **File ▸ Keyboard shortcuts**. Pick a command, press **Set in cordinal**
or **Set from any program**, then press the key. Letters need Ctrl or Alt so that typing still
works; F-keys, media keys and the numpad can be used alone. Keys set "from any program" work even
when another window is in front. **Clear** removes a command's keys and **Reset all** brings back the
defaults. Your keys are saved in `settings.ini` under `[Keys]` and `[Global keys]`, as lines such as
`Ctrl+Shift+K=Playback > Next`. On Windows, every menu item also has its own underlined access key.

## Guide

- [Library](#library)
- [Album editions](#album-editions)
- [Playlists and the queue](#playlists-and-the-queue)
- [Sound](#sound)
- [Tags](#tags)
- [Covers](#covers)
- [Lyrics and artist info](#lyrics-and-artist-info)
- [Scrobbling](#scrobbling)
- [Discord status](#discord-status)
- [Devices and iPod sync](#devices-and-ipod-sync)
- [Network speakers and sharing](#network-speakers-and-sharing)
- [Seek bar, waveform and moodbar](#seek-bar-waveform-and-moodbar)
- [Album colours](#album-colours)
- [Lightweight mode](#lightweight-mode)
- [Layouts, packs and helpers](#layouts-packs-and-helpers)
- [Your data](#your-data)

### Library

Add folders in **Preferences ▸ Library ▸ Library folders** or from the **Library** menu. cordinal
scans every subfolder and then watches for changes. New files are added on their own, moved files
keep their history, and an unplugged drive loses nothing. Empty placeholder files are skipped until
something fills them. If you want to review new music before it joins the library, turn on the Inbox
in **Preferences ▸ Library**.

**Library ▸ Add Steam soundtracks** finds the soundtracks you have installed through Steam, on both
Windows and Linux. It runs in the background and can be cancelled from the Library menu. When a
soundtrack comes in more than one format, you choose whether to keep one copy of each song or all of
them. Files already in your library are skipped. Folders where games keep their sound effects (such
as `sfx` or `sound`) are left out unless you turn on **Include game sound effects** in
**Preferences ▸ Library**.

### Album editions

Many albums have more than one edition: the original, a remaster, a deluxe or regional release. cordinal
can show these under one album tile with an edition selector. It recognizes clear edition names and
MusicBrainz release-group tags, and each edition keeps its own files, track list and artwork.

**Library ▸ Album editions** lets you group, split, rename and compare editions, and choose the
preferred edition and cover. An optional MusicBrainz lookup can fill in missing release-group ids;
turn it on in Preferences (on Linux, in **Library ▸ Album editions**). Everything else works offline.
Playlists always keep pointing at the exact files they list, and `%edition%` in a title format shows
which edition a track belongs to.

### Playlists and the queue

Playlists open as tabs. **Ctrl+Z** undoes a change to a playlist (adding, removing, moving, sorting
or clearing) and **Ctrl+Y** redoes it. If a tag edit came last, Ctrl+Z undoes that instead.

**Auto-playlists** fill themselves from rules, such as "rated 4 stars or more, not played this month".
Rules can use tags, file details, ratings, play and skip counts, dates, length, BPM, loudness and
more, in nested ALL/ANY groups with an optional limit. Auto-playlists share one tab: click it to
switch between them, make a new one or edit the rules (on Windows, Down opens this menu from the
keyboard). To give each its own tab, turn off **Preferences ▸ View ▸ One tab for auto-playlists**.

While an album is playing, the **Now Playing** tab shows only that album. Type in its search box to
see the whole list again. **Preferences ▸ View ▸ Now Playing shows the playing album** turns this off.

**Columns.** Drag a divider to resize a column (double-click it to reset), and drag a heading to move
it. Click a heading to sort, click again to reverse, and a third time to go back to the original
order. Right-click the headings to choose which columns show.

**Up next.** **View ▸ Up next** (Ctrl+Shift+Q) shows the queue: what you queued, then what the
playlist plays after it. Drag tracks to reorder them, or drag them in from the list. Delete removes
tracks, Alt+Up and Alt+Down move them, and Enter plays one now.

**Choose by taste.** **Playback ▸ Choose by taste** is off until you turn it on. Then track shuffle
and **Keep playing like this** lean toward songs you finish, rate, replay and come back to, and toward
the sound of the songs you like. A skip while you are just flipping through songs counts for less,
and one just before you close cordinal doesn't count. About one pick in seven is still random, so
forgotten songs keep coming up. Album shuffle still plays each album in order. The next five picks
wait in the queue under **Picked by taste**: remove one to skip it, choose **Keep** to make sure it
plays, or click **New picks** for others.

**Resume where you left off.** With this on (the clock button on the toolbar), cordinal remembers where
you stopped in up to twelve songs. When you come back, **Pick up where you left off** lets you carry on
from that point. It also suggests a few albums and songs you used to play a lot but haven't heard in a
while. **Playback ▸ Play history** lists the songs you played to the end, newest first.

When **Playback ▸ Speed** or **Tempo** is changed, the Duration and BPM columns and the total time
show what you will actually hear.

### Sound

**Output mode.** **Preferences ▸ Output ▸ Output** has three choices:

- **Shared** plays through the Windows mixer, alongside other programs.
- **Exclusive** gives cordinal the device to itself.
- **ASIO** (Windows only) plays through an installed ASIO driver. **ASIO driver...** on the same page
  chooses the driver and opens its own settings.

Exclusive and ASIO play each track at its own sample rate, bit-perfect at 100% volume, with the EQ,
effects and ReplayGain turned off. If the device can't be opened, cordinal falls back to Shared and
the status bar says why. Click the output name in the status bar to switch modes. On Linux, Exclusive
asks PipeWire to run at the track's sample rate.

**Native DSD** (Windows only, off by default) plays DSF and DSDIFF files as DSD to a DAC that accepts
it: over DoP in Exclusive mode, or ASIO's own DSD mode when the driver has one. Volume (other than
mute), EQ, effects and ReplayGain don't apply to a DSD stream; the status bar names the rate and transport. A DAC
without DoP support plays noise, so leave it off unless your DAC supports DSD.

**Crossfade.** **Preferences ▸ Playback ▸ Crossfade between tracks** fades one track into the next (1
to 8 seconds, off by default). Tracks from the same album stay gapless unless you also turn on
**Crossfade within an album**.

**Volume.** **Preferences ▸ Output** sets the volume curve. It can also turn off cordinal's own volume
control, so only Windows, PipeWire or your amplifier sets the level.

**Surround.** **Upmix to surround** (off by default) plays stereo through every speaker of a 5.1 or 7.1
setup in Shared mode. Voices stay at the front while the room sound spreads around you.

**Headphones.** **Preferences ▸ Headphones** keeps separate EQ, crossfeed and ReplayGain settings for
each output. Some headsets also offer battery, sidetone, auto power-off and onboard EQ controls. On
Linux, these need read and write access to the headset's hidraw device.

**Effects.** The bitcrusher is in **Playback ▸ Bitcrush**, and its settings are in
**Preferences ▸ Effects**.

**ReplayGain.** **Preferences ▸ ReplayGain** can scan the selection, the playlist or the whole
library. Results are kept in the library. **Playback ▸ ReplayGain ▸ Write ReplayGain tags** saves them
into the files.

### Tags

Press **Alt+Enter** to edit the tags, cover and lyrics of one track or many. Artist, album and genre
fields suggest names already in your library, so spellings stay consistent. Every save is all or
nothing, so a failure leaves the file as it was, and Ctrl+Z undoes the last ten edits.

- **Edit album tags...** edits every track of an album when you select just one of its songs.
- **Organize files** renames and moves music based on its tags, with a preview and undo.
- **Library ▸ Find missing information** lists tracks with missing tags. You can also type
  `missing:year`, `missing:genre` or `missing:any` in the search box.
- When a track has no title, artist or track number, the editor suggests them from the file name
  (such as `03 - Artist - Title`).
- **Make album...** turns the selected tracks into one album, or tidies an existing one: title,
  album artist, year, genre, cover, track order and numbering.
- **Tag album from MusicBrainz...** finds matching releases, shows every change before you apply it,
  and saves it as one edit you can undo. Turn it on or off in **Preferences ▸ Online ▸ Album tags from
  MusicBrainz**.

All of these are in the **Edit** menu or a track's right-click menu.

**Finding duplicates.** **Library ▸ Find duplicates...** finds two kinds:

- **Identical audio**: the same sound, even with different tags or file names. Songs that haven't
  been measured yet can be measured from this window.
- **Same song**: the same artist, title and length in different files, such as a FLAC and an MP3.

The best copy is listed first. Extra copies of identical audio are ticked for you; same-song copies
are not, since you may want both. **Move checked to Recycle Bin** removes the ticked files and moves
their ratings, plays and playlist places to the copy you keep. **Undo clean-up** brings them back
while cordinal is still open.

**Converting.** **Library ▸ Convert...** (also on a track's right-click menu) converts the selected
songs, or the whole library when nothing is selected, to `.cord`, FLAC, WAV, MP3 or AAC (`.m4a`), with
their tags and covers. cordinal has its own encoders, so there is nothing else to install. Choose:

- **Copies, and keep the originals**: next to each original, or in a folder you pick, where each
  album keeps its own folder. The library and the originals stay as they are.
- **Replacements for the originals in the library**: the library switches to each new file and keeps
  its plays, ratings and playlists. The originals go to the Recycle Bin, or stay where they are, as
  set in **Preferences ▸ Convert**. **Library ▸ Undo last conversion** brings them back.

Every new file is read back and checked before it counts (lossless ones sample for sample), and
existing files are never overwritten. MP3 and AAC can come from any song cordinal plays, and play
without gaps between tracks. Lossless formats need a lossless original: FLAC, WAV, AIFF, ALAC, WavPack,
Monkey's Audio, TTA or `.cord` (mono or stereo, up to 24 bits for the last four), since a lossless copy
of a lossy file would only be bigger.

### Covers

The quickest way to add a cover is to drag a JPEG or PNG onto one of the album's tracks. You can also
right-click a track and choose **Cover ▸ Choose image file...** or **Cover ▸ Paste image from
clipboard**. The cover applies to the whole album.

To look online, right-click and choose **Cover ▸ Search online...** (in the tag editor, **Find cover
online...**). You can paste an image, drag one in from a browser, or pick a file. **Google Images**
opens a search in your browser; cordinal never contacts Google itself. Deezer, iTunes and MusicBrainz
results appear too, once you turn those sources on in **Preferences ▸ Online**.

By default, a chosen cover is **shown in cordinal only** and no music file changes. **Restore original
cover** undoes it. You can instead save covers as `cover.jpg` in the album folder, or embed them in
the files.

**Library ▸ Fetch missing covers...** searches for every album without a cover and lets you check the
results before saving.

**Disc art.** If an album folder (or its `Artwork` or `Scans` folder) has `cd.png`, `cd2.png`,
`disc.png` or `vinyl.png`, the disc peeks out from behind the cover.

**Preferences ▸ View ▸ Cover change** (off by default) adds a transition: the old cover breaks up like a
badly saved JPEG, and the new one appears block by block.

### Lyrics and artist info

The lyrics panel (**View ▸ Lyrics panel**, Ctrl+L) shows lyrics from the song's tags or from a `.lrc`
file beside it. With an online source turned on, it asks LrcLib and then lyrics.ovh, and prefers timed
lyrics. Nothing is saved until you right-click the panel and choose **Save lyrics to tags** or **Save
as .lrc file**.

**View ▸ Artist info...** shows the start of the artist's English Wikipedia article, links to
MusicBrainz, Wikipedia and their website, and their albums from MusicBrainz. Albums you already have
are marked **In library**; tick **Only albums not in my library** to see what you are missing.

### Scrobbling

Open **File ▸ Scrobbling...** to connect Last.fm, ListenBrainz or Libre.fm, in any combination.
ListenBrainz needs your user token. Last.fm and Libre.fm need an API key and secret (**Get API key**
opens the sign-up page), then approval in your browser and **Finish sign-in**.

- Your sign-in details are encrypted with your Windows account, or kept in a private file on Linux.
- Listens made offline are queued and sent later.
- You can pause scrobbling, or turn off "now playing" updates.
- A song counts after half its length or four minutes of listening. Last.fm and Libre.fm also need it
  to be longer than 30 seconds.

### Discord status

**Preferences ▸ Scrobbling ▸ Discord status** shows what you are playing on Discord, either the
**Song** or the **Album**. It talks only to the Discord app on your computer and shows nothing while
paused. It is off by default.

Discord can only show pictures from the web. Covers you picked online work as they are. For covers
from your own files, cordinal uses a matching cover from your online sources, or you can set an
upload command in `settings.ini`:

```ini
[Scrobbling]
DiscordCoverUpload=C:\Tools\upload.exe --json
```

cordinal runs the command with the image's path, uses the first https address it prints, and
remembers it, so each image is uploaded only once.

### Devices and iPod sync

**Copy to folder or device** copies music to SD cards, music players and phones.

**Library ▸ Sync to iPod...** copies the selected tracks (or the current playlist) to a classic iPod,
keeping a backup of its previous database. It accepts MP3, AAC, ALAC, WAV, AIFF and audiobooks. iPhones,
iPads and copy-protected purchases are not supported.

### Network speakers and sharing

**Preferences ▸ Output ▸ Network speaker...** (on Linux, **Output device...**) lists the UPnP/DLNA
players on your network, such as TVs, receivers, smart speakers or media software on another
device, and plays there instead. EQ, ReplayGain, crossfade and effects all carry over, and the queue
stays in cordinal. The speaker plays a few seconds behind. Choose **This computer** to switch back.
If the speaker never starts, allow cordinal through the firewall on private networks.

**Web server... ▸ Share with network players** lets players on your home network browse and play your
library. DLNA has no passwords, so anyone on that network can use it while it is on. It is off by
default and never reachable from the internet.

In the **browser player**, **⋯ ▸ Download to this device** saves a song's original file, and
**Download** above an album or playlist saves all of it as one ZIP (up to 4 GB).

### Seek bar, waveform and moodbar

**Preferences ▸ View ▸ Seek bar** sets how the seek bar looks:

- **Progress bar**, **Waveform**, **Moodbar** (a stripe coloured by the sound) or **Mood waveform**.
- **Seek bar shows** the **Whole song** or **Live audio**, a moving picture of what you hear. Radio
  always shows live audio.
- **Seek bar glow** adds a soft halo that pulses with the beat. **60 frames a second** makes motion
  smoother at a small CPU cost.
- **Moodbar colours**, **Waveform colour**, **Waveform reflection** and **Smooth seek bar** change its
  colours and shape.
- **Fade the waveform when pausing** fades it to amber when you pause.
- **Wind behind the track list** (off by default) draws soft ribbons that drift with the music.
- **Smooth scrolling** (off by default) eases the library list instead of jumping.

The track list's **Mood** column shows each track's moodbar.

### Album colours

**Preferences ▸ View ▸ Album colours** sets how much of the window takes on the colour of the playing
album's cover: just the **Player background** (the default), the **Whole interface**, or **Off**.
**Album colour strength** goes from Subtle to Bold. Text always stays readable.

### Lightweight mode

**View ▸ Lightweight mode** (Ctrl+Shift+L, or the icon by the clock) keeps the music playing while
cordinal uses as little memory and CPU as it can. The window closes, but the music and every effect
keep going. In **Preferences ▸ Lightweight mode** you choose what stays on screen: just the tray icon,
the mini player or the compact window.

You can still control the music from any program:

- **Win+Alt+Space** (or clicking the tray icon) opens a small song picker. Type part of a song, album
  or artist and press Enter to play it now, or Shift+Enter to play it next.
- **The numpad** works as a remote while Num Lock is off: 5 pauses, 4 and 6 skip, 8 and 2 change the
  volume, 7 and 9 seek, 1 mutes, 3 shuffles, 0 opens the song picker and `.` brings the window back.
- **Right-click the tray icon** for playback controls, Up next, playlists, volume and more.

Ctrl+Shift+L, Esc in the mini player, or the numpad's `.` brings the window back. If another program
already uses one of these keys, cordinal tells you. Lightweight mode works the same way on Linux,
where keys from other programs go through the desktop's shortcut settings.

### Layouts, packs and helpers

**View ▸ Window size** offers Compact, Big mode plus and Big mode, and the mini player
(Ctrl+Shift+M). Big mode has panels you can drag, split and dock. From the keyboard, F6 moves between
panel groups and Ctrl+Up or Ctrl+Down moves a panel between areas. Reopen closed panels from **View ▸
Big mode panels**. **Startup defaults** chooses the size and layout to use next time.

**Packs** are folders or `.cordpack` files (a ZIP) that add themes, columns, group headers, title
formats, shortcuts, EQ and layout presets, auto-playlists and fonts. Nothing in a pack runs as a
program. Install and turn them on in **View ▸ Packs**, or drop one on the window. Turning a pack off
removes what it added. Layout presets, and **Reset the layout**, are in **View ▸ Customize layout ▸
Presets**. **Make a pack from my setup** in **View ▸ Packs** writes your current look as a new pack,
with a commented example of every file; edit it, then choose **Reload packs**. Every file is optional:

| File | What it holds |
|---|---|
| `pack.ini` | `[pack]`: `name`, `version`, `author`, `description`, `format = 1` |
| `theme.ini`, `themes/*.ini` | `[theme]`: `name`, `base` (light or dark), `font`, `size`, `playing mark`, `rating mark`, `message prefix`; `[palette]`: `#RRGGBB` colours by role, held to the built-in themes' contrast rules; `[waveform]`: `played`, `unplayed`; `[sounds]`: WAV or FLAC files for sound effects |
| `columns.ini` | A section per column: `title`, `format`, `sort`, `width`, `align = right`. A `[set.name]` section with `name` and `show` (columns left to right) is a column set |
| `groups.ini` | `[groups]`: `by` (tracks whose `by` text matches share a header), `title`, `subtitle`, `right`, `cover = no`; `%group_tracks%` and `%group_length%` describe the group |
| `formats.ini` | `[formats]`: `window title`, `status bar`, `now playing`, `now playing 2`, `tray tip` |
| `fields.ini`, `functions.tf` | `[fields]`: `name = format` adds `%name%`; `$define(name, args, body)` adds a title-format function |
| `keys.ini` | `[keys]` and `[global keys]`: `Ctrl+Alt+N = Playback > Next`, a command as the command palette names it |
| `eq.ini`, `eq/*.ini` | A section per preset: `name`, `bands` (ten gains in dB) |
| `layout.ini`, `layouts/*.ini` | `[layout]`: `name`, `player`, `waveform size`, then `1` or `0` for each part |
| `autoplaylists/*.txt`, `fonts/*.ttf` | Auto-playlist rules (offered in the Playlist menu), and fonts loaded only for cordinal |

**Helpers** are separate programs that cordinal can run, such as visualizers in their own windows
(**View ▸ Helpers ▸ Visualizers**). A helper can only read the audio, and it stops when cordinal
closes.

### Your data

| | Windows | Linux |
|---|---|---|
| Library, playlists, statistics | `%APPDATA%\cordinal` | `$XDG_DATA_HOME/cordinal` |
| Settings | `%APPDATA%\cordinal` | `$XDG_CONFIG_HOME/cordinal` |
| Cover and waveform caches | `%LOCALAPPDATA%\cordinal` | `$XDG_CACHE_HOME/cordinal` |

cordinal only goes online for features you turn on: radio, scrobbling, Discord covers, the browser
player, the album-edition lookup, and the cover, lyrics and artist-info sources.

**Back up.** **File ▸ Back up library and settings** saves everything into an ordinary folder. **File
▸ Restore backup** brings it back, even on a new computer where the music is in a different place.

**Your library looks after itself.** Every save checks that `library.cdb` was written whole, and keeps the
save before it as `library.cdb.bak`. Once a day cordinal also keeps a dated copy in the `library-history`
folder beside it: the last seven days, plus the first copy of each of the last three months. If the
library file is ever damaged (a failing disk can change a byte years later), cordinal notices when it
starts and opens the previous save, or else the newest dated copy, and tells you which one it used.

**Export your music.** **File ▸ Export collection...** saves your music, or the artists, albums and
playlists you pick, into one `.zip` file, with covers, lyrics and playlists. **File ▸ Import
collection...** unpacks such a file into your library. It never overwrites a file, so importing twice
only adds what is missing. Ratings and play counts are not included; use a backup for those.

**Leaving cordinal.** Nothing is locked in:

- **Tags** are saved in the music files, in standard fields.
- **Ratings and play counts** can be written into the files: select tracks, then right-click ▸
  **Rating ▸ Write ratings and play counts to files**. They are saved as `FMPS_RATING` and
  `FMPS_PLAYCOUNT` (and `POPM` for MP3).
- **Playlists** save as `.m3u8` with Ctrl+S.
- **A backup folder** holds `statistics.csv`, `history.csv` and every playlist, all readable without
  cordinal.

## Linux (preview)

The Linux version shares its library, decoders, tags and playback engine with the Windows version, and
has the same features and look. It is less tested, and the Windows version remains the reference.

It runs on Wayland or X11 and plays through PipeWire or PulseAudio. It is one static file with no
shared libraries at all: it brings its own C library, talks to the display and sound servers itself,
draws its own text, and has HTTPS built in (BearSSL). It runs on any 64-bit x86 Linux kernel, old
distributions and musl systems such as Alpine included. To install or update it:

```sh
curl -fsSL https://github.com/imhavingasuperbadday/cordinal/releases/latest/download/install.sh | sh
```

The download is checked against the release checksum. To remove cordinal, replace the final `sh` with
`sh -s -- --uninstall` (your library and settings are kept). `sh -s -- --dry-run` shows what it would
do. The same file runs on every distribution: there is no separate musl build and nothing to install
besides cordinal itself.

**Omarchy and Waybar.** On Omarchy, and other Hyprland or Sway desktops with Waybar, turn on **Show on
the top bar** (Preferences ▸ Startup, or the View menu) to put cordinal on the top bar. It adds a small
marked module to your Waybar config and removes it again when you turn it off. Click it to show
cordinal, right-click to pause, and middle-click to skip.

## Building

**Windows** needs Visual Studio 2022 (or its Build Tools) with the C++ x64 workload:

```bat
build.bat unpacked
```

This makes a quick development build. Plain `build.bat` also compresses the program with the pinned
UPX packer, which it downloads and checks. Releases use cordinal's own packer: `build.bat cdpack`, which
also needs [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)'s `clang++` on `PATH` and Python 3.14
(see [cdpack/README.md](cdpack/README.md)). Every mode writes `build\cordinal.exe`.

Builds are incremental: only changed files and the files that include them are recompiled. New `.cpp`
files in the existing source folders are found on their own; a new folder needs an entry in
[tools/build.ps1](tools/build.ps1).

**Linux** needs CMake, a C compiler and a C++20 compiler, and no libraries:

```sh
cmake -S . -B build-linux && cmake --build build-linux
```

`-DCORDINAL_PORTABLE=OFF` uses the system's C++ runtime instead of building it in.
`-DCORDINAL_SELAWIK=ON` builds in the Selawik font for the Windows look (it is off by default because
its licence, SIL OFL 1.1, is not GPL-compatible).

### Testing

On Windows, `test.bat` builds cordinal, makes any missing test files (with FFmpeg on `PATH`) and runs
every test suite:

```bat
test.bat
```

| Command | What it does |
|---|---|
| `test.bat -Headless` | Runs only the tests that need no window or sound device |
| `test.bat -List` | Lists every suite and what it needs |
| `test.bat -NoBuild -Suite search,sorting` | Runs chosen suites against the existing build |
| `test.bat -NoBuild -Changed` | Runs the suites that cover your uncommitted changes |
| `-Seed 42 -Repeat 10` | Repeats a randomized run |

Results go to a new folder under `build\test-results\`. A failing check prints its file and line, and
a command to run it again. `pwsh -File tools\check-size.ps1` reports the program size; a packed build
over 2,500,000 bytes fails. The memory-budget suite checks the player's RAM use.

On Linux, the build also makes `cordinal-core-test`; run it with no arguments to see its commands.
`cordinal --check-layout` compares the window with the shared layout.

### Fuzzing and sanitizers

CI's `linux-sanitizers` job builds the core with clang under AddressSanitizer and
UndefinedBehaviorSanitizer, runs the core self-tests, decodes the official Opus conformance vectors
(every packet's range-coder state must match the encoder's, and libopus's `opus_compare` must accept
the output), and fuzzes every file parser with libFuzzer. To run the same on Linux:

```sh
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCORDINAL_FUZZ=ON
cmake --build build-fuzz --target cordinal-core-test fuzzers cordinal-opus-conformance
bash linux/smoke-test.sh build-fuzz results/core
tools/opus-conformance.sh build-fuzz results/opus
tools/fuzz-media.sh build-fuzz results/fuzz 60
```

A crash leaves its input in `results/fuzz/crashes/<target>/`; `build-fuzz/fuzz-<target> <file>`
replays it.

## Contributing and support

- **Bugs:** open an [issue](https://github.com/imhavingasuperbadday/cordinal/issues/new/choose) with
  your cordinal version, your Windows or Linux version, and the steps to reproduce it. On Windows,
  attach any crash report from `%LOCALAPPDATA%\cordinal\crashes`.
- **Format requests:** use the *Format request* template, with a short sample file if you can share it.
- **New features:** open an issue before you start, so your time isn't wasted on something that
  doesn't fit.
- **Security problems:** report them privately, as [SECURITY.md](SECURITY.md) explains.
- **Code, tests and docs:** read [CONTRIBUTING.md](CONTRIBUTING.md) first, and sign the [CLA](CLA.md)
  with your first pull request.

## License

cordinal is free software under the GNU GPL, version 3 or later ([LICENSE](LICENSE)).
[THIRD_PARTY.md](THIRD_PARTY.md) lists the outside code and data it includes.
