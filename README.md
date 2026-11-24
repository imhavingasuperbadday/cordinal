<p align="center"><img src="docs/icon.png" alt="cordinal icon" width="128"></p>

<h1 align="center">cordinal</h1>

<p align="center">
A fast, native music player packed into one ~2 MB executable.<br>
No installer, no runtime, no codec packs, no telemetry. Just drop it anywhere and play.
</p>

<div align="center">

Current release: **0.1.19** · `cordinal.exe`, 2,252,288 bytes<br>
**[Download](https://github.com/imhavingasuperbadday/cordinal/releases/latest)** ·
[Quick start](#quick-start) · [Guide](#guide) · [Linux](#linux-preview) · [Building](#building) ·
[Issues](https://github.com/imhavingasuperbadday/cordinal/issues)

</div>

**cordinal** is a free, open-source (GPLv3+) music player and library for Windows, with a native Linux
preview. It's written in C++20 straight against Win32 — no framework, no web view, no bundled DLLs. It
opens instantly, plays pretty much any audio format you throw at it using its own built-in decoders, and
leaves your music right where you keep it.

## Why cordinal?

- **Everything in one file.** Decoders, encoders, a 10-band EQ, ReplayGain scanner, tag editor, CD
  ripper, duplicate finder, web player, and scrobbler — all inside a single ~2 MB `.exe`.
- **Your files don't get mangled.** Every tag, playlist, and library write is atomic: if anything fails
  mid-write, your original file stays untouched byte for byte. Multi-file tag edits and conversions can
  be undone.
- **Actually tested on broken files.** Every decoder, tag parser, and cover-art reader is fuzzed under
  AddressSanitizer and UndefinedBehaviorSanitizer in CI on every push. The Opus decoder passes the
  official conformance vectors. Nothing ships unless CI is green.
- **Zero lock-in.** Your music stays in your own folders. Tags, ratings, play counts, and playlists can
  all be written in standard formats that other players read.
- **Quiet by default.** cordinal doesn't touch the network until you turn on a feature that needs it, and
  it collects nothing about you.

## What's inside

- **Playback:** Gapless playback or crossfade. Bit-perfect Exclusive and ASIO output, plus native DSD
  for DACs that take it. Independent speed and tempo control (phase-locked vocoder). ReplayGain with a
  built-in loudness scanner. 10-band EQ with headphone (AutoEq) profiles, crossfeed, reverb, night mode,
  bitcrusher, and 5.1/7.1 stereo upmixing. Internet radio, a browser player for your phone, and
  UPnP/DLNA network speakers.
- **30+ built-in formats:** FLAC (including Ogg FLAC), WAV, AIFF, MP3, MP4/M4A, Ogg Vorbis, Opus, AAC
  (including HE-AAC v1/v2), ALAC, WMA, APE, WavPack, Musepack, AC-3/E-AC-3, TTA, Matroska audio, DSD, Apple CAF, Sun AU, Creative
  VOC, Amiga 8SVX, CRI ADX, GSM 06.10, G.722, G.726, ComputerCraft DFPWM, CUE sheets, tracker modules,
  MIDI, retro game music (NES, SNES, Game Boy, Genesis, C64), and cordinal's own lossless `.cord`
  format.
- **Library & playlists:** Watched folders that track new and moved files without losing history. Track
  list, cover grid, albums, artists, and column browser. Album editions (remasters, deluxe releases, etc.)
  grouped under one cover. Playlists, rule-based auto-playlists, drag-to-reorder queue, M3U/M3U8/PLS,
  Steam soundtrack import, device copying, and classic iPod sync. Search ignores accents and takes
  inline filters with autocomplete (`year:1990..1999 format:flac rating:>=4 played:never lyrics:...`),
  and **Playlist ▸ Save search as auto-playlist** keeps one.
- **Tags, ripping & conversion:** Edit tags, covers, and lyrics for one song or hundreds with undo.
  Rename and organize files from tags. Convert to FLAC, WAV, MP3, AAC, or `.cord` (as copies or undoable
  replacements). Rip audio CDs to FLAC or `.cord` with two-pass sector verification. Find exact-audio or
  same-song duplicates and missing tags. Optional online covers, lyrics, artist info, web saving, and
  Release Radar.
- **History & taste:** Half-star ratings, favorites (hearts), play/skip counts, play log, and a year in
  review. Ready-made local Mixes and a shuffle mode that learns your taste. Last.fm, ListenBrainz, and
  Libre.fm scrobbling, plus Discord status. Sync all of it between your PCs through any folder you
  already sync, with no server or account.
- **Make it yours:** Light and Dark themes with optional album-cover colour tinting. Layouts from Big
  mode down to a mini player or tray-only Lightweight mode. Waveform and moodbar seek bars. Shareable
  `.cordpack` customization packs, external visualizer helpers, and a command palette (`Ctrl+Shift+P`)
  for every command and setting.

## Quick start

1. Grab `cordinal.exe` from
   [Releases](https://github.com/imhavingasuperbadday/cordinal/releases/latest) and run it.
2. Add your music folders in **Preferences ▸ Library ▸ Library folders** (`Ctrl+,`) or the **Library**
   menu. cordinal won't scan anything until you point it at a folder.
3. Double-click a track to play. Hit `Ctrl+F` to search, or `Ctrl+Shift+P` to jump to any command or
   setting.
4. Online features are off out of the box — turn on what you want in **Preferences ▸ Online** and
   **Preferences ▸ Scrobbling**.

**Updating:** just replace `cordinal.exe` with the new download. Your library, playlists, ratings, and
settings carry over automatically. Moving to another PC? Use **File ▸ Back up library and settings**.

Runs on 64-bit Windows 10 and 11. On Linux, see [Linux (preview)](#linux-preview).

Every setting in **Preferences** (`Ctrl+,`) has an explanation right beside it, a **Reset** button when
changed from default, and a search box that filters across all pages (**All settings** shows everything
at once).

## Keyboard shortcuts

| Key | Action |
|---|---|
| `Space` | Play / pause |
| `Enter` / double-click | Play |
| `Left` / `Right` | Seek 5 seconds |
| `Ctrl+Left` / `Ctrl+Right` | Previous / next |
| `Ctrl+Enter` / `Ctrl+Q` | Play next / add to queue |
| `Ctrl+Shift+Q` | Up next (queue) |
| `Ctrl+F` | Search |
| `Alt+Enter` | Edit tags |
| `Ctrl+Z` / `Ctrl+Y` | Undo last playlist change or tag edit / redo playlist change |
| `Ctrl+S` | Save playlist as M3U8 |
| `Ctrl+N` / `F2` / `Ctrl+W` | New / rename / close playlist |
| `Ctrl+D` | Favorite selected tracks (or clear hearts) |
| `Ctrl+E` | Equalizer |
| `Ctrl+L` | Lyrics panel |
| `Ctrl+Shift+M` | Mini player |
| `Ctrl+Shift+L` | Lightweight mode |
| `Win+Alt+Space` | Song picker from any app (in Lightweight mode) |
| `Ctrl+Shift+P` | Command palette |
| `Ctrl+,` | Preferences |

Rebind anything in **File ▸ Keyboard shortcuts** — pick a command, choose **Set in cordinal** or
**Set from any program** (global hotkey), and press the key. Letters require `Ctrl` or `Alt`; F-keys,
media keys, and numpad keys work on their own. Shortcuts are stored in `settings.ini` under `[Keys]`
and `[Global keys]` (e.g. `Ctrl+Shift+K=Playback > Next`).

## Guide

Everything below is optional reading — expand whatever you're curious about.

<details>
<summary><strong>Library, Steam soundtracks, web saving &amp; online discovery</strong></summary>

### Watched folders &amp; Inbox
Add folders in **Preferences ▸ Library ▸ Library folders** or the **Library** menu. cordinal scans
subfolders and watches for changes: new files appear on their own, moved files keep their stats,
unplugged drives don't lose anything, and empty placeholder files are skipped. Turn on **Inbox** in
**Preferences ▸ Library** if you'd rather review new music before it enters your library.

### Steam soundtracks
**Library ▸ Add Steam soundtracks** finds installed Steam soundtracks on Windows and Linux in the
background (cancel anytime from the Library menu). When a soundtrack ships in multiple formats, pick
one copy per song or keep all of them. Existing tracks are skipped, and game SFX folders (`sfx`,
`sound`) are ignored unless you enable **Include game sound effects**.

### Save music from the web &amp; playlist imports
**Library ▸ Save music from the web...** downloads pasted links into your first library folder as real
local files (`Artist\Album\[track] Title`). Supports SoundCloud (tracks, sets, likes, artist pages),
YouTube (videos and playlists), and Spotify (tracks, albums, playlists — matched to the same recording
on YouTube without breaking Spotify's encryption, with the source noted in the comment tag).

- Fragmented MP4 streams from SoundCloud and YouTube are remuxed losslessly into standard `.m4a` files
  (never re-encoded) with full tags and embedded cover art. Unusual streams fall back to saving as-is
  with `cover.jpg` beside them.
- Playlist links automatically create a playlist tab in the exact original order (including songs you
  already owned). Re-running the link updates that tab (undoable with `Ctrl+Z`) and never overwrites a
  playlist you made yourself.
- Already-saved tracks are skipped, interrupted downloads resume next time, and permanently gone tracks
  are remembered until you click **Forgot what failed**.
- **Import file...** brings in Spotify account data exports (`YourLibrary.json` for Liked Songs + saved
  albums, `Playlist1.json` for any playlist including private ones) and CSV playlists from Exportify,
  TuneMyMusic, and Soundiiz (with `Playlist Name` splitting). Drag and drop a file, paste its path, or
  paste the text. Tracks you already own are matched locally and skipped; saved tracks are hearted
  automatically.
- Asks before going online the first time (**Preferences ▸ Online ▸ Save music from the web**).

### Release Radar, discovery &amp; online search
- **Library ▸ New releases by your favorite artists...** checks MusicBrainz for albums and EPs from the
  last six months by the artists you play and heart most. **Save selected** / **Save all** pulls the
  album tracklist through the web-save queue as properly tagged albums. Nothing is fetched until you
  open it, and nothing downloads until you click Save.
- **Discover outside my library** (**Preferences ▸ Online**) lets **Keep playing like this**
  occasionally slip in an unowned track by a related artist (picked via MusicBrainz, audio matched on
  YouTube) as a temporary preview. It isn't counted as a play and gets deleted after playing or skipping
  unless you press `Ctrl+Alt+K` (**Library ▸ Keep the discovered song**).
- **Online song search** (**Preferences ▸ Online**, off by default) adds an **Online (not in your
  library)** pane below local `Ctrl+F` matches (up to 15 results, and up to 5 in `Win+Alt+Space`). Play
  as a preview until you save, or switch **When playing a search result** to **Play and save
  automatically**. Right-click an album result to **Save full album to library...**.

</details>

<details>
<summary><strong>Album editions</strong></summary>

Remasters, deluxe releases, and regional editions can live under one album tile with an edition
switcher. cordinal detects common edition names and MusicBrainz release-group tags while keeping each
edition's files, tracklist, and artwork separate.

Use **Library ▸ Album editions** to group, split, rename, compare, or pick a preferred edition and
cover. Everything works offline; an optional MusicBrainz lookup fills in missing release-group IDs.
Playlists always point at exact files, and `%edition%` works in title formats.

</details>

<details>
<summary><strong>Playlists, Mixes, queue &amp; taste shuffle</strong></summary>

- **Tabs &amp; undo:** Playlists open as tabs. `Ctrl+Z` undoes playlist edits (add, remove, move, sort,
  clear) or your last tag edit; `Ctrl+Y` redoes playlist edits.
- **Auto-playlists:** Rule-based playlists using tags, file info, ratings, favorites, play counts,
  dates, BPM, loudness, and nested ALL/ANY groups. They share one tab by default (turn off
  **Preferences ▸ View ▸ One tab for auto-playlists** to separate them).
- **Favorites &amp; Mixes:** Hit `Ctrl+D` to heart tracks (or un-heart if all selected are hearted).
  **Playlist ▸ Mixes** builds instant local playlists: **Favorites**, **On Repeat** (most played in the
  last 30 days), **Forgotten Favorites** (hearted or 4+ stars, unplayed for 90 days), **Recently Added**,
  and **Mix like this song** (matching genre or similar tempo).
- **Now Playing focus:** While playing an album, the **Now Playing** tab focuses on that album (type in
  search to see everything again, or disable in **Preferences ▸ View**).
- **Columns:** Drag dividers to resize (double-click to reset), drag headings to reorder, click to sort
  (asc / desc / original), right-click headings to toggle columns.
- **Up next (`Ctrl+Shift+Q`):** Shows your queue followed by what's coming from the playlist. Drag to
  reorder, `Alt+Up`/`Alt+Down` to move, `Delete` to remove, `Enter` to play now.
- **Choose by taste (`Playback ▸ Choose by taste`, off by default):** Biases shuffle and **Keep playing
  like this** toward songs you finish, rate, heart, and replay — ignoring quick skips while flipping
  through tracks, and keeping ~1 in 7 picks random so forgotten songs still surface. The next five picks
  sit in **Picked by taste** where you can keep, skip, or reroll them.
- **Resume where you left off:** The toolbar clock remembers your spot in up to 12 tracks and suggests
  old favorites you haven't played lately. **Playback ▸ Play history** shows your recent full plays.
- Changing **Speed** or **Tempo** updates the Duration, BPM, and total time columns to match what you'll
  actually hear.

</details>

<details>
<summary><strong>Sound, Exclusive/ASIO, DSD, EQ &amp; ReplayGain</strong></summary>

- **Output modes** (**Preferences ▸ Output**, or click the output name in the status bar):
  - **Shared:** standard OS mixer alongside other apps.
  - **Exclusive:** gives cordinal direct access to the device (on Linux, asks PipeWire for the track's
    native sample rate).
  - **ASIO** (Windows): plays through an installed ASIO driver.
  - Exclusive and ASIO play bit-perfect at each track's native sample rate (100% volume, EQ/effects/RG
    bypassed), falling back to Shared with a status bar note if the device can't be opened.
- **Native DSD** (Windows, off by default): sends DSF/DSDIFF directly over DoP (Exclusive) or native
  ASIO DSD. Leave off unless your DAC supports DSD, as non-DoP DACs will play noise.
- **Crossfade:** 1–8 seconds between tracks (**Preferences ▸ Playback**), keeping tracks on the same
  album gapless unless you also enable **Crossfade within an album**.
- **Volume &amp; Surround:** Configurable volume curve (or disable internal volume control completely).
  **Upmix to surround** (Shared mode) spreads stereo across 5.1/7.1 while keeping vocals centered.
- **Headphones:** Per-output EQ, crossfeed, and ReplayGain profiles, plus battery/sidetone/auto-off/EQ
  controls for supported headsets (needs hidraw read/write access on Linux).
- **Effects &amp; ReplayGain:** Built-in bitcrusher (**Playback ▸ Bitcrush**), reverb, night mode, and
  ReplayGain scanning (**Preferences ▸ ReplayGain**; write tags to files via **Playback ▸ ReplayGain ▸
  Write ReplayGain tags**).

</details>

<details>
<summary><strong>Tags, duplicates, converting &amp; CD ripping</strong></summary>

- **Tag editor (`Alt+Enter`):** Edit tags, covers, and lyrics for one track or hundreds, with library
  autocomplete for artists/albums/genres and filename guessing for untagged tracks. Atomic writes with
  10-step `Ctrl+Z` undo. Also in the **Edit** / right-click menus: **Edit album tags...**, **Organize
  files** (rename/move from tags with preview &amp; undo), **Make album...**, **Tag album from
  MusicBrainz...** (preview before applying), and **Library ▸ Find missing information** (or search
  `missing:year`, `missing:genre`, `missing:any`).
- **Lint (`Library ▸ Lint my library...`):** One review screen for albums split in two, near-duplicate
  artist names, garbled or legacy-encoded tags, and cover-art problems. Every fix is previewed, only
  checked ones apply, and **Edit ▸ Undo last tag edit** takes back the whole run.
- **Duplicates (`Library ▸ Find duplicates...`):** Finds both **Identical audio** (same sound regardless
  of tags/filenames) and **Same song** (same artist/title/length across formats like FLAC + MP3). Moving
  checked copies to the Recycle Bin migrates their ratings, plays, and playlist spots to the kept copy,
  with **Undo clean-up** while cordinal stays open.
- **Converting (`Library ▸ Convert...`):** Built-in encoders for `.cord`, FLAC, WAV, MP3, and AAC
  (`.m4a`) with tags and covers — no external tools needed. Save as **Copies** or **Replacements**
  (swapping in-place in your library while keeping stats/playlists, undoable via **Library ▸ Undo last
  conversion**). Every output is read back and verified (sample-for-sample for lossless); lossless
  targets require lossless inputs.
- **CD ripping (`Library ▸ Rip CD...`):** Reads CD-Text offline or looks up discs on MusicBrainz (with
  optional Cover Art Archive front covers), rips to `<folder>\<artist>\<album>\` in FLAC or `.cord`,
  and adds tracks to your library. Every sector is read twice and re-read on mismatch; drive read-offset
  correction lives in **Preferences ▸ Conversion**. With **Preferences ▸ Online ▸ AccurateRip** on, each
  track is checked against other people's rips of the same disc, **Detect offset** finds your drive's read
  offset, and every rip writes an `<Album>.log`. Existing files are never overwritten. On Linux, your
  user needs read access to the drive (typically the `cdrom` or `optical` group).

</details>

<details>
<summary><strong>Covers, lyrics &amp; artist info</strong></summary>

- **Covers:** Drag a JPEG/PNG onto any track, or right-click ▸ **Cover ▸ Choose image file / Paste image
  from clipboard**. **Cover ▸ Search online...** lets you paste, drop from a browser, open a Google
  Images tab (cordinal never contacts Google), or pull from Deezer, iTunes, and MusicBrainz when enabled.
  By default, picked covers are stored in cordinal only without touching your audio files (or save as
  `cover.jpg` / embed in tags). **Library ▸ Fetch missing covers...** batch-searches missing artwork for
  review. Drop `cd.png`, `cd2.png`, `disc.png`, or `vinyl.png` in an album folder (or `Artwork`/`Scans`)
  for peeking disc art.
- **Lyrics (`Ctrl+L`):** Reads embedded tags and sidecar `.lrc` files. With online sources enabled,
  checks LrcLib then lyrics.ovh (preferring synced lyrics). Nothing is written until you right-click and
  save to tags or `.lrc`.
- **Artist info (`View ▸ Artist info...`):** Shows a Wikipedia intro, links, and the artist's
  MusicBrainz discography with your owned albums highlighted. Filter to missing albums, inspect
  tracklists, preview tracks, or save full missing albums straight to your library.

</details>

<details>
<summary><strong>Scrobbling &amp; Discord status</strong></summary>

- **Scrobbling (`File ▸ Scrobbling...`):** Connect Last.fm, ListenBrainz, and/or Libre.fm in any
  combination. Credentials are encrypted with your Windows account (or stored in a private file on
  Linux). Offline plays queue up and submit later. Tracks count after half their length or 4 minutes
  (plus >30s for Last.fm/Libre.fm). **Import history...** pulls your past ListenBrainz or Last.fm
  listens into play counts and last-played dates.
- **Bringing your stats along (`File ▸ Import playback statistics...`):** Reads an iTunes/Apple Music
  `Library.xml`, MusicBee CSV/TSV exports and XSPF playlists, shows what matched before anything
  changes, and **Edit ▸ Undo** takes the import back.
- **Discord status (`Preferences ▸ Scrobbling`, off by default):** Shows the playing song or album via
  the local Discord client (hidden when paused). Discord only shows covers that live on the web, so
  covers picked from online results work automatically. For embedded covers, cover files, and albums
  you made in cordinal, turn on **Upload covers for Discord**: it sends the cover the player shows to
  litterbox.catbox.moe, which deletes it after three days, and reuses that address for two. Otherwise
  cordinal looks the album up in your enabled online sources, or runs your own uploader from
  `settings.ini`:
  ```ini
  [Scrobbling]
  DiscordCoverUpload=C:\Tools\upload.exe --json
  ```
  It passes the image path, grabs the first `https://` URL printed, and caches it so each cover uploads
  once.

</details>

<details>
<summary><strong>Devices, classic iPod sync &amp; network speakers</strong></summary>

- **Devices &amp; iPod:** **Copy to folder or device** copies music to SD cards, DAPs, and phones.
  **Library ▸ Sync to iPod...** syncs MP3, AAC, ALAC, WAV, AIFF, and audiobooks to classic iPods while
  backing up the previous iPod database (no iPhones/iPads or DRM files).
- **Network speakers:** **Preferences ▸ Output ▸ Network speaker...** (Linux: **Output device...**)
  streams to UPnP/DLNA speakers, TVs, and receivers with your EQ, ReplayGain, crossfade, and effects
  intact. Allow cordinal through the firewall on private networks if a speaker won't start.
- **Browser player &amp; DLNA sharing:** **Web server... ▸ Share with network players** exposes your
  library over DLNA on your LAN (off by default, never exposed to the internet; note DLNA has no
  passwords). In the browser player, download individual original files or grab an entire album/playlist
  as a ZIP (up to 4 GB).

</details>

<details>
<summary><strong>Seek bar, album colours &amp; Lightweight mode</strong></summary>

- **Seek bar (`Preferences ▸ View`):** Progress bar, Waveform, Moodbar, or Mood waveform — showing the
  whole song or live audio (radio is always live). Optional beat glow, 60 fps motion, reflection,
  pause-to-amber fade, drifting background wind ribbons, smooth scrolling, and a **Mood** track-list
  column.
- **Album colours (`Preferences ▸ View`):** Tint the player background, the whole interface, or nothing
  from the playing cover, with Subtle-to-Bold strength and guaranteed text readability.
- **Lightweight mode (`Ctrl+Shift+L`):** Closes the main window and keeps memory/CPU trimmed while playback
  and effects keep running. Keep just the tray icon, the mini player, or the compact window. Control music
  from anywhere with `Win+Alt+Space` (song picker), the numpad with Num Lock off (`5` pause, `4`/`6`
  skip, `8`/`2` volume, `7`/`9` seek, `1` mute, `3` shuffle, `0` picker, `.` restore), or right-clicking
  the tray icon.

</details>

<details>
<summary><strong>Layouts, packs &amp; helpers</strong></summary>

- **Layouts:** Use the **Layout** menu to pick a preset (Classic, Player at the bottom, Player on the
  side, Waveform studio, Minimal, or pack presets), start from a blank canvas, dock the player on any
  edge, toggle individual parts, or save up to 8 custom layouts (**My layout 1–8**). **Layout ▸ Window
  size** switches between Compact, Big mode, Big mode plus, and Mini player (`Ctrl+Shift+M`). In Big
  mode, drag/split/dock panels freely (`F6` switches panel groups, `Ctrl+Up`/`Ctrl+Down` moves panels).
- **Packs (`.cordpack` or folders):** Pure data — nothing in a pack executes code. Install via
  **View ▸ Packs** or drag-and-drop onto the window; toggle off to cleanly undo. Use **Make a pack from
  my setup** to export your current look with commented templates for every optional file:
  - `pack.ini` — metadata (`name`, `version`, `author`, `description`, `format = 1`)
  - `theme.ini`, `themes/*.ini` — palette (`#RRGGBB`, held to built-in contrast rules), font, marks,
    waveform colours, WAV/FLAC sound effects
  - `columns.ini`, `groups.ini` — custom columns, column sets, and group headers
  - `formats.ini`, `fields.ini`, `functions.tf` — window/status/tray formats, `%field%` definitions, and
    `$define(...)` functions
  - `keys.ini`, `eq.ini`, `eq/*.ini`, `layout.ini`, `layouts/*.ini` — shortcuts, EQ presets, and layouts
  - `autoplaylists/*.txt`, `fonts/*.ttf` — auto-playlist rules and app-local fonts
- **Helpers:** External programs (like standalone visualizers under **View ▸ Helpers ▸ Visualizers**)
  that only receive audio and exit when cordinal closes.

</details>

<details>
<summary><strong>Your data, backups &amp; leaving cordinal</strong></summary>

| Data | Windows | Linux |
|---|---|---|
| Library, playlists, stats | `%APPDATA%\cordinal` | `$XDG_DATA_HOME/cordinal` |
| Settings | `%APPDATA%\cordinal` | `$XDG_CONFIG_HOME/cordinal` |
| Cover &amp; waveform caches | `%LOCALAPPDATA%\cordinal` | `$XDG_CACHE_HOME/cordinal` |

- **Self-healing library:** Every save verifies `library.cdb` was written intact and keeps the previous
  copy as `library.cdb.bak`, plus daily snapshots in `library-history/` (last 7 days + first of each of
  the last 3 months). If disk corruption ever flips a byte, cordinal automatically recovers from the
  newest intact copy and tells you.
- **Backup &amp; export:** **File ▸ Back up library and settings** writes a plain folder you can restore
  even on a machine with different music paths. **File ▸ Export collection...** bundles chosen artists,
  albums, or playlists (with covers and lyrics) into one `.zip`; **Import collection...** unpacks it
  without overwriting existing files.
- **Sync between PCs:** **Library ▸ Sync with other PCs...** keeps plays, skips, ratings, hearts,
  playlists, and places left partway through songs the same on every PC, through a folder Syncthing,
  Nextcloud, Dropbox, or a USB stick already keeps in step. Pick the same folder on each PC. Each one
  appends to a small log of its own there and only reads the others', so the sync tool never sees two
  versions of one file, and `library.cdb` never goes near the folder. Tracks match by their path under
  your library folders, then audio hash, then artist, album, title, and length, so a Windows desktop and
  a Linux laptop with music in different places line up. Plays made after joining add up (with each
  listen, so the year in review counts both PCs); counts from before join by the highest, so a copied
  library isn't counted twice. For everything else the newest change wins, and a playlist closed on one
  PC can be reopened on the others with `Ctrl+Shift+T`.
- **No lock-in:** Tags live in standard fields inside your audio files. Ratings and play counts can be
  written to files (`FMPS_RATING`, `FMPS_PLAYCOUNT`, MP3 `POPM`) from the right-click **Rating** menu.
  Playlists export to `.m3u8` (`Ctrl+S`), and backups include plain `statistics.csv` and `history.csv`.

</details>

## Linux (preview)

The Linux port shares the same engine, decoders, library, tags, and look as Windows (Windows is still
the reference and gets more mileage).

What makes the Linux build unusual: it's **one completely static x86-64 binary with zero shared
libraries**. It brings its own C library, talks straight to Wayland/X11 and PipeWire/PulseAudio, draws
its own text, and bundles BearSSL for HTTPS. BearSSL stops at TLS 1.2, so a server that only takes
TLS 1.3 gets asked again through the system's `curl` when one is installed. Same binary runs on any 64-bit x86 Linux distro — old
glibc, Alpine/musl, whatever.

It does its own text shaping too, so Japanese, Korean, Arabic, Hebrew, Thai and Devanagari titles
render properly without HarfBuzz. On Wayland it follows fractional scaling (125%, 150%...), and
X11 picks up your session's scale too. Input methods like fcitx5 and ibus work on both: you compose
right in the search box and other text fields, with the candidate list at the caret.

It stays small, too. An idle window settles around 5 MB: memory goes back to the system while you
aren't using the player, a window that sits still keeps none of its pixels, and the code that plays
music is linked together so playback keeps about 3 MB of it resident.

Install or update:

```sh
curl -fsSL https://github.com/imhavingasuperbadday/cordinal/releases/latest/download/install.sh | sh
```

The installer verifies the release checksum. Swap the final `sh` for `sh -s -- --uninstall` to remove it
(your library and settings stay untouched), or `sh -s -- --dry-run` to preview.

**Waybar / Omarchy:** Turn on **Show on the top bar** (**Preferences ▸ Startup** or **View**) to add a
cleanly marked Waybar module (click to show, right-click to pause, middle-click to skip). Turning it off
removes the module again.

## Building

**Windows** requires Visual Studio 2022 (or Build Tools) with the C++ x64 workload:

```bat
build.bat unpacked
```

That gives you a fast incremental dev build. Plain `build.bat` packs it with pinned UPX. Official
releases use cordinal's own packer (`build.bat cdpack`), which also needs
[LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)'s `clang++` on `PATH` and Python 3.14 (see
[cdpack/README.md](cdpack/README.md)). Every mode outputs `build\cordinal.exe`. New `.cpp` files in
existing folders are picked up automatically; new folders go in [tools/build.ps1](tools/build.ps1).

**Linux** needs CMake, a C compiler, and a C++20 compiler — no external libraries:

```sh
cmake -S . -B build-linux && cmake --build build-linux
```

Pass `-DCORDINAL_PORTABLE=OFF` to use the system C++ runtime instead, or `-DCORDINAL_SELAWIK=ON` to
bundle the Selawik font (off by default since SIL OFL 1.1 isn't GPL-compatible).

### Testing

On Windows, `test.bat` builds cordinal, generates missing test fixtures (needs FFmpeg on `PATH`), and
runs every suite:

```bat
test.bat
```

| Command | What it does |
|---|---|
| `test.bat -Headless` | Skip window/audio-device tests |
| `test.bat -List` | List all suites and their requirements |
| `test.bat -NoBuild -Suite search,sorting` | Run specific suites against the current build |
| `test.bat -NoBuild -Changed` | Run suites covering your uncommitted changes |
| `-Seed 42 -Repeat 10` | Repeat a randomized run |

Logs land under `build\test-results\`. Failures print `file:line` and a copy-pasteable reproduce
command. Check binary size with `pwsh -File tools\check-size.ps1` (packed builds fail above 2.5 MB),
and check RAM use with the `memory-budget` suite.

On Linux, the build also produces `cordinal-core-test` (run with no args for usage) and supports
`cordinal --check-layout`.

### Fuzzing and sanitizers

CI's `linux-sanitizers` job compiles the core with clang under ASan + UBSan, runs the core self-tests,
verifies the official Opus conformance vectors (range-coder state + `opus_compare`), and fuzzes every
file parser with libFuzzer. Run it locally on Linux:

```sh
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCORDINAL_FUZZ=ON
cmake --build build-fuzz --target cordinal-core-test fuzzers cordinal-opus-conformance
bash linux/smoke-test.sh build-fuzz results/core
tools/opus-conformance.sh build-fuzz results/opus
tools/fuzz-media.sh build-fuzz results/fuzz 60
```

Crashes save their reproducer to `results/fuzz/crashes/<target>/`; replay with
`build-fuzz/fuzz-<target> <file>`.

## Contributing & support

- **Bugs:** Open an [issue](https://github.com/imhavingasuperbadday/cordinal/issues/new/choose) with
  your cordinal version, OS version, and repro steps (on Windows, attach crash logs from
  `%LOCALAPPDATA%\cordinal\crashes`).
- **Format requests:** Use the *Format request* template and attach a short sample if you can.
- **Feature ideas:** Open an issue before writing code so you don't waste time on something out of scope.
- **Security:** Report privately per [SECURITY.md](SECURITY.md).
- **PRs:** Read [CONTRIBUTING.md](CONTRIBUTING.md) and sign the [CLA](CLA.md) on your first PR.

## License

GNU GPL v3 or later ([LICENSE](LICENSE)). Third-party code and data are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
