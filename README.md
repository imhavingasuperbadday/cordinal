<p align="center"><img src="docs/icon.png" alt="cordinal icon" width="128"></p>

<h1 align="center">cordinal</h1>

<p align="center">
A small, fast music player and library for Windows, with a Linux preview.<br>
One 1.8 MB program. No installer, no runtime, no codecs to add.
</p>

<div align="center">

Current release: **0.1.14** · `cordinal.exe`, 1,951,744 bytes<br>
**[Download](https://github.com/imhavingasuperbadday/cordinal/releases/latest)** ·
[Getting started](#getting-started) · [Guide](#guide) · [Building](#building) ·
[Testing](#testing) · [Issues](https://github.com/imhavingasuperbadday/cordinal/issues)

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

- Gapless playback or a crossfade between tracks, exclusive and ASIO bit-perfect output, speed and tempo control
- Sample-rate conversion that preserves treble and suppresses aliasing
- ReplayGain with a built-in loudness scanner
- Ten-band equalizer with headphone (AutoEq) profiles, crossfeed and per-output settings
- Effects: reverb, night mode and a bitcrusher, each of which can be switched off
- Stereo upmixed to 5.1 or 7.1 speakers, with the original left and right kept on the front pair
- Internet radio, and a browser player for listening on your phone and downloading the original files
- Network speakers (UPnP/DLNA) as an output, and your library shared with network players such as foobar2000

### Formats

FLAC (also in Ogg), WAV, AIFF, MP3, MP4/M4A, Ogg Vorbis, Opus, AAC, ALAC, WMA, APE, WavPack,
Musepack, AC3 and E-AC-3, TTA, Matroska audio, DSD, Apple CAF, Sun AU, Creative VOC, Amiga 8SVX,
CRI ADX, GSM 06.10, G.722, G.726, ComputerCraft DFPWM, CUE sheets, tracker modules, MIDI, video
game music (NES, SNES, Game Boy, Genesis, Commodore 64) and cordinal's own lossless `.cord`.

### Library and playlists

- Watched folders: new files join on their own, moved files keep their history, unplugged drives
  lose nothing
- Track list grouped by album, cover grid, albums with tracks, artists, and a column browser
- BPM detection while tracks are measured, filled in when the BPM column is shown
- An optional Inbox to review new music before it joins the library
- Album editions (remaster, deluxe, regional...) sharing one tile, with an edition selector
- Static playlists, rules-based auto-playlists, Play next and a queue you can reorder by dragging
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
- Choose by taste: track shuffle and Keep playing like this follow what you finish, rate, replay and
  come back to
- Native Last.fm, ListenBrainz and Libre.fm scrobbling with offline queues
- Discord status showing the song or album you're playing

### Make it yours

- Light, Dark, or follow Windows, coloured by the playing album's cover as much or as little as you
  like
- Window sizes from Big mode down to a mini player, with dockable panels and layout presets
- Lightweight mode: music in the background on little memory, with a song picker and a numpad
  remote
- Columns, title formats, and a seek bar that can be a waveform or a moodbar
- Packs: folders or `.cordpack` files that add columns, layouts, EQ presets and themes
- Helpers: separate programs such as visualizers, running in their own windows
- A command palette (Ctrl+Shift+P) that finds any command or setting, including the ones on a
  track's right-click menu such as Play next and Show in Explorer
- On Windows, every menu item has its own underlined access key, so Alt and one letter per menu run it

## Getting started

1. Download `cordinal.exe` from
   [Releases](https://github.com/imhavingasuperbadday/cordinal/releases/latest) and run it.
2. Add your music folders in **Preferences ▸ Library ▸ Library folders** (Ctrl+,), or from the
   **Library** menu. Nothing is scanned until you do.
3. Double-click a track to play it. Press **Ctrl+F** to search and **Ctrl+Shift+P** to find any
   command or setting by name.
4. cordinal keeps every online feature off until you turn it on. Choose the sources you want in
   **Preferences ▸ Online** and **Preferences ▸ Scrobbling**.

To update, replace `cordinal.exe` with the new download. Your library, playlists, ratings and
settings stay where they are. To move them to another computer, use **File ▸ Back up library and
settings** first.

cordinal runs on 64-bit Windows 10 and 11. For Linux, see [Linux](#linux-preview).

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
| Ctrl+Shift+L | Lightweight mode: keep playing in the background with the window put away |
| Win+Alt+Space | In Lightweight mode, from any program: pick a song |
| Ctrl+Shift+P | Command palette |
| Ctrl+, | Preferences |

Every command can have keys of your own in **File ▸ Keyboard shortcuts**. Pick a command, press
**Set in cordinal** or **Set from any program**, then press the key: a letter needs Ctrl or Alt so
typing still works, while F-keys, media keys and the numpad can be used on their own. Keys from any
program work while you are in another window (a numpad key on its own does so in Lightweight mode).
A key another command had moves to the new one, **Clear** takes a command's keys away, and
**Reset all** brings back cordinal's own. The menus show your keys, and they are kept in
`settings.ini` under `[Keys]` and `[Global keys]` as `Ctrl+Shift+K=Playback > Next`.

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
- [Album colours](#album-colours)
- [Lightweight mode](#lightweight-mode)
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

Auto-playlists share one tab, where the first of them sits, named after the one you opened last
(⚡ Top rated ▾). Click it again for a menu of all of them, **New auto-playlist...** and **Edit
rules...** (on Windows, Down opens it from the keyboard; Ctrl+Tab steps through every auto-playlist
too). **Preferences ▸ View ▸ One tab for auto-playlists** gives each its own tab instead.

While a song from an album plays, the **Now Playing** tab lists only that album, and moves on with
playback to the next album. Type in its search box to look through the whole list; songs without an
album always show all of it. **Preferences ▸ View ▸ Now Playing shows the playing album** turns this
off.

In the track list, drag a divider to resize the column on its left (double-click it to give the column
its own width back), and drag a column's heading to move it. Click a heading to sort by that column, again
to reverse the sort, and a third time to go back to the list's own order. Right-click the headings
to choose which columns show.

Ctrl+Z undoes changes to a playlist (adding, removing, moving, sorting, clearing) and Ctrl+Y redoes
them, for the session; Ctrl+Z takes back a tag edit instead when that came last.

**View ▸ Up next** (Ctrl+Shift+Q) opens the queue beside the list, or as Big mode's Up next panel.
It lists what you queued, numbered in play order, then what the playlist plays after it. Drag
queued tracks to reorder them, or drag tracks from the list onto it to queue them at that spot.
Delete removes the selected tracks, Alt+Up and Alt+Down move them, Enter or a double-click plays one
now, and right-click has Move to the top or bottom, Remove and Clear the queue.

**Playback ▸ Choose by taste** (also **Preferences ▸ Playback**) is off until you turn it on. Track
shuffle and **Keep playing like this** then follow songs you come back to, finish, rate and rewind
into, and lean toward the loudness, dynamics and mood of what you finish. A skip is judged by its
moment: one just before you stop or close cordinal doesn't count, one while you flip through songs
counts for less than one right after a song you heard through, and each song is compared with
others of its genre that came up in the same kind of moment. Artists and genres count by how often
you finish them, not by how many of their songs you have, and about one track-shuffle pick in seven
stays plain shuffle, so songs it passed over still come up. Mood comes from a waveform or a
ReplayGain scan, so it starts once those exist. Album shuffle still plays each album in order.

While taste is choosing (track shuffle or Keep playing like this, not Repeat track), its next five
picks wait in the queue under **Picked by taste**, after anything you queued yourself, and a new one
joins as each plays. Remove a pick to skip it, drag it up among yours or choose **Keep** to make sure
it plays, or click **New picks** for others. Turning taste off takes the picks out again.

With **Resume where you left off** on (the toolbar's clock button), cordinal remembers where you
left each song you moved on from partway, up to twelve of them. When you come back it opens **Pick
up where you left off** (also in the Playback menu, or right-click the clock button): click a song
to carry on from that moment. Drag it by a heading to move it; it opens there next time. A song
drops off once you play it to its end, and its × removes it. Choosing that song anywhere else, such
as the track list, also carries on from its place rather than starting it over, and its row in the
list is underlined, at half strength, as far as you got.
Below them, **Haven't heard in a while** offers up to three albums and four songs you played a lot
and then let go quiet. The gap depends on how much you played them: a song with 100 plays comes back
after a month, one with 3 plays after nearly six months, and an album by its plays per track. × on
one hides it until you play it again. **Playback ▸ Play history** lists the songs you played to the
end, newest first, by day; click one to play it.

While **Playback ▸ Speed** or **Tempo** is off normal, the track list's Duration and BPM columns
and its total time say what you will hear: at 1.1x tempo a 31:35 mix at 148 BPM shows 28:43 and 163.

### Sound and ReplayGain

The bitcrusher is in **Playback ▸ Bitcrush**; its depth, sample rate and mix are in
**Preferences ▸ Effects**. **Preferences ▸ Headphones** keeps separate EQ, crossfeed and ReplayGain
headroom for each output; remembering an output's EQ is opt-in. Supported headset hardware also
offers battery, sidetone, automatic power-off, Bluetooth do-not-disturb and onboard EQ controls, and
Apply writes only the choices you select. On Linux, hardware access needs normal read/write access
to the headset's hidraw interface.

**Preferences ▸ Playback ▸ Crossfade between tracks** fades the end of one track into the next when
playback moves on by itself (1 to 8 seconds; off by default). Tracks of the same album stay gapless
unless you turn on **Crossfade within an album**, and a track you start yourself, a radio stream or
a section loop still changes at once. **Preferences ▸ Output** sets the volume curve (how the
slider's travel turns into loudness) and can turn off cordinal's own volume control, sending the
music at full level so only Windows, PipeWire or your amplifier sets it; mute still works.
**Upmix to surround** (off by default) plays stereo from every speaker of a 5.1 or 7.1 device in
shared output: the front pair keeps the original left and right, the center takes what both sides
share at half level, the subwoofer takes that below 120 Hz, and the surrounds take what differs
between the sides (100 Hz to 7 kHz, 12 ms later, in opposite polarity), so ambience spreads around
you while voices stay in front. The status bar shows the layout in use. Exclusive and ASIO output
stay stereo and bit-perfect.

**Preferences ▸ Output ▸ Output** is **Shared** (through the Windows mixer, alongside other programs),
**Exclusive** (the Windows device to itself) or **ASIO** (Windows only). Exclusive and ASIO play each
track at its own sample rate, bit-perfect at 100% volume with EQ, crossfeed, effects and ReplayGain off;
the status bar says so. ASIO plays through an installed ASIO driver's first two outputs in the driver's
own sample format and buffer size, without the Windows audio engine and with no SDK or extra file:
**ASIO driver...** on the same page picks the driver and opens its own settings (buffer size, clock)
while it plays. A paused player lets the driver go after two seconds so other programs can use it.
When the driver can't be opened, output falls back to shared and the status bar says why. Clicking
the output in the status bar steps through Shared, Exclusive and ASIO. On Linux, Exclusive asks
PipeWire to run at the track's rate, the native counterpart of ASIO.

**Preferences ▸ ReplayGain** has **Scan selection**, **Scan playlist** and **Scan library**. Scan
library remeasures every available library track, including earlier scans and failed attempts,
regardless of the current playlist or search; missing files and Inbox tracks are skipped.
**Cancel scan** appears while a manual scan runs. Results are stored in the library;
**Playback ▸ ReplayGain ▸ Write ReplayGain tags** saves them into the selected files.

### Tags

Edit every field, the cover and lyrics for one track or hundreds (Alt+Enter). Artist, album artist,
album, genre and composer suggest values already in your library as you type, so spellings stay
consistent; a new value is always allowed. Every write is atomic, so a failure leaves the file as it
was, and Ctrl+Z undoes the last ten edits. **Organize files** renames and moves music from its tags,
with a preview and undo.

**Edit album tags...** (the Edit menu or a track's right-click menu) opens the editor on every track
of the selected tracks' albums, so one selected song is enough to give a whole album its year,
genre or album artist. **Library ▸ Find missing information** lists the library tracks missing a
title, artist, album, album artist, year, genre, track number or composer, or any of the basic ones;
it is a search, so you can also type `missing:year`, `missing:genre`, `missing:any` and so on in the
search box, and combine it with other words. Select the tracks it finds and press Alt+Enter to fill
the gap; each track leaves the list once it has the tag.

**Library ▸ Find duplicates...** lists every song the library holds more than once, in two kinds of
group. *Identical audio* is the same sound whatever the tags or file names: FLAC and `.cord` files
carry a fingerprint of their audio, and every other format gets one when cordinal measures its
loudness, so the window offers **Measure them** for songs not measured yet. *Same song* is the same
artist and title within a second of the same length, in different files, such as a FLAC and an
MP3. Each group lists its best copy first (lossless before lossy, then the higher bitrate, the
fuller tags and the longer history). In identical groups the other copies are checked for you; in
same-song groups nothing is, since a lossy copy may be wanted on purpose. **Move checked to Recycle
Bin** sends the checked files to the Recycle Bin (the trash on Linux) and moves their plays, skips,
ratings, listening history and playlist places to the copy kept; a song with every copy checked is
left alone, and the playing song is never moved. **Undo clean-up** brings the files back with their
history, while cordinal is still open.

**Make album...** (the Edit menu or a track's right-click menu) turns the selected tracks into one
album, or tidies an album you already have. It starts from what the tracks share and gives the
album a title, album artist, year, genre, disc number and cover (a file, or **Find cover online**);
tracks by different artists start as a compilation by Various Artists. Put the tracks in order with
**Move up** and **Move down** (Alt+Up and Alt+Down), **Order** (by track number, title or file name,
or reversed) and **Remove**; **Number the tracks in this order** writes the track numbers to match.
**Set every track's artist to the album artist** is there for albums whose songs credit guests. On
Windows the window stays open beside the list, so **Add selected** brings in more tracks. Saving
writes only the tags that change, as one edit that **Undo last tag edit** takes back.

**Tag album from MusicBrainz...** (the Edit menu or a track's right-click menu) looks the selected
album up on MusicBrainz and lists the releases it found, best match first, with how well each one's
track titles and lengths fit your files. Choose a release to see every tag it would change, track by
track; nothing is written until **Apply**, which writes only those tags (with the MusicBrainz release
and release-group ids) as one edit that **Undo last tag edit** takes back. Track lengths pick the
right pressing; when no release fits clearly, none is chosen for you, and tracks the release doesn't
have are left alone. It asks before its first lookup, and **Preferences ▸ Online ▸ Album tags from
MusicBrainz** turns it on or off.

When a track has no title, artist or track number in its tags (the list then shows its file name),
the editor offers them from the file name, such as `03 - Artist - Title`, as gray hints in the empty
fields. **Fill empty title, artist and track number from the file name** writes them on Save, for
one track or many; type your own value to use that instead, or untick the box.

### Covers

The quickest way to give an album a cover is to drag a JPEG or PNG file onto one of its tracks (or
onto cordinal with the album selected). A track's right-click menu has **Cover ▸ Choose image
file...** and **Cover ▸ Paste image from clipboard** too. Each of these applies to the whole album at
once, with no window to go through, and keeps the cover the way **Preferences ▸ Online** says.

To look online, right-click an album or some of its tracks and choose **Cover ▸ Search online...**
(in the tag editor: **Find cover online...**, beside **Choose cover...**). Use **From file...**,
paste an image with Ctrl+V, or drag one in from a browser or a folder; a copied image address works
too. **Google Images** opens your browser on a search for the album: copy the picture you like
there, then paste it. cordinal never contacts Google itself.

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

**Preferences ▸ View ▸ Cover change** (off by default) makes the player's cover crunch into a badly
saved JPEG when the next song has a different cover: blocks, banding and grain, fading to an old
black-and-white photograph (**Black and white JPEG**) or a crunched colour one (**Colour JPEG**).
Then the new cover opens out of the middle, block by block. Songs from the same album keep their
cover. With Windows' "Animation effects" turned off, the new cover simply appears.

### Lyrics and artist info

The lyrics panel (**View ▸ Lyrics panel**, Ctrl+L) shows lyrics from the song's tags or a `.lrc`
file beside it. With a lyrics source on in **Preferences ▸ Online**, it asks LrcLib, then
lyrics.ovh, for songs that have none. Timed lyrics are preferred, only lyrics for the same title are
used, and the panel says where they came from. They are written nowhere until you right-click the
panel and choose **Save lyrics to tags** (undoable) or **Save as .lrc file** (never replacing one).

**View ▸ Artist info...** (or a track's right-click menu) shows the selected or playing track's
artist with the opening of their English Wikipedia article and buttons for MusicBrainz, Wikipedia
and the official site. It shows an artist only when exactly one MusicBrainz artist has that name.
Below the article is their discography: their albums and EPs as MusicBrainz lists them, oldest
first, with the ones already in your library marked **In library**. Tick **Only albums not in my
library** to see what you're missing; double-click one (click on Linux) to open its MusicBrainz page.

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

### Network speakers and sharing

**Preferences ▸ Output ▸ Network speaker...** (on Linux, **Output device...**) lists the UPnP/DLNA players
on your network (TVs, receivers, smart speakers, or foobar2000, Kodi or BubbleUPnP on another device)
and plays there instead of on this computer. cordinal sends the speaker what it would play here, as
lossless 16-bit audio, so EQ, ReplayGain, crossfade, gapless playback and effects all carry over, and
the queue stays in cordinal. Pause and resume reach the speaker; a seek gives it a fresh stream. The
speaker plays a few seconds behind, and the position shown follows what it reports. Choose **This
computer** to come back. If the speaker never starts, allow cordinal through the firewall on private
networks.

**Web server... ▸ Share with network players** lets players on your home network browse your albums,
artists, playlists, all songs and most played, and play each song as its own file (or as WAV, for a
format the player doesn't know). DLNA has no pairing, so anyone on that network can browse and play
while it is on; it is off until you turn it on, and never reachable from outside.

In the browser player, a song's **⋯ ▸ Download to this device** saves its file exactly as stored, named
*Artist - Title*; **Download** above an album or playlist saves all of its files in one ZIP (up to
4 GB). A song inside a cue sheet or a game rip has no file of its own to download.

### Seek bar, waveform and moodbar

**Preferences ▸ View ▸ Seek bar** picks how the seek bar looks:

- **Progress bar**, **Waveform**, or a bar coloured by its sound. Each moment's balance of bass and
  treble turns its hue to one side of the theme's accent or the other, and louder moments stand out
  more: **Moodbar** draws a flat stripe, **Mood waveform** keeps the waveform's shape.
- **Seek bar shows** the **Whole song**, or **Live audio**, a moving picture of what is being heard
  (optionally **with progress**). Radio always shows live audio, and seeking works either way. The
  live bar lifts on each beat, so a loud, compressed master still visibly moves.
- **60 frames a second** (off by default) draws the live waveform and the seek bar's glow 60 times
  a second instead of 30, for smoother motion; it uses a little more CPU.
- **Seek bar glow** draws a soft halo that pulses with the beat, brighter where the song is loud.
- **Fade the waveform when pausing** (on by default) fades the waveform between its playing colour
  and amber instead of switching at once. If your waveform colour is itself amber or yellow, paused
  turns the text colour instead, so you can still tell the two apart.
- **Wind behind the track list** (off by default) sends soft ribbons drifting across the right of the
  list while music plays, curling, swelling and gusting with it: wide and slow for bass, short and
  quick for treble. They pass behind the track text, melt into the list's edges instead of being cut
  off, stop when you pause, and stay off on Windows while its animation effects are turned off.
- **Moodbar colours**: Theme, Vivid (the whole colour wheel), Spectrum (red bass, green mids, blue
  treble), Warm, Cool, Colour-blind friendly (blue to orange) or Single colour. **Mood waveform
  colours** can give the Mood waveform a scheme of its own.
- **Waveform colour** replaces the theme's colour with a named one (kept readable on the player);
  the moodbar's Theme, Vivid and Single colour schemes turn from it. **Waveform unplayed part**
  shows what is still to play in the theme's grey or in that colour faded.
- **Waveform reflection** is **Mirrored** below the centre line, **Upper half**, or **Full height**
  (mirrored as bright below the line as above; the Moodbar stripe keeps the upper half).
- **Smooth seek bar** rounds the outline and blends the colours.
- **Smooth scrolling** eases the library toward where the mouse wheel or Page Up and Page Down take
  it instead of jumping. Scrollbar drags and keyboard focus still move at once. It is off by default.

Over the whole song, both moodbar looks draw a playhead line in the playback state's colour: amber
while paused (the text colour if the waveform is amber or yellow), red when playback will stop or
the track failed. The track list's **Mood** column shows each track's moodbar; tracks not seen
before fill in while nothing else is being prepared, and a ReplayGain scan prepares them all.

### Album colours

**Preferences ▸ View ▸ Album colours** sets how much of the window takes the colour of the playing
album's cover. **Player background** (the default) washes the player and an open album's panel with
it. **Whole interface** turns everything toward it: the list, the panels, the borders, the selection,
and the accent that marks what's playing. **Off** keeps every surface neutral. **Album colour
strength** goes from Subtle to Bold. Text always stays readable: a colour that would cost it contrast
is held back. Album colours apply to Light and Dark; other themes keep their own look.

### Lightweight mode

**View ▸ Lightweight mode** (Ctrl+Shift+L, or the icon by the clock) keeps cordinal playing in the
background on as little memory and CPU as it can: the window goes away and its caches with it, while
the music, the equalizer, reverb and every other effect carry on exactly as they were. Choose in
**Preferences ▸ Lightweight mode** what stays on screen: nothing but the icon by the clock, the mini
player, or the compact window. Background library work (measuring ReplayGain beyond the next songs,
drawing waveforms) waits, and a moment later the memory cordinal no longer needs goes back to
Windows.

Without the window, music is still a key away, from any program:

- **Win+Alt+Space** (or a click on the icon by the clock) opens a small song picker. Type a few
  letters of a song, album or artist and press Enter: the song plays now, then what was playing
  carries on. Shift+Enter puts it next instead. With nothing typed, Space pauses and Left and Right
  skip, and the picker also lists Up next and your playlists. Press Win+Alt+Space again for the full
  window.
- **The numpad** is a remote: 5 pauses, 4 and 6 skip, 8 and 2 set the volume, 7 and 9 seek, 1 mutes,
  3 shuffles, 0 opens the song picker and `.` brings the window back. It works while Num Lock is
  off, so turning Num Lock on gives you the numbers back; Preferences can make it always on or off,
  and with Always the numpad's + - * / and Enter can have commands too. Give any numpad key another
  command in **File ▸ Keyboard shortcuts**: its keys "from any program" are the remote's.
- **Right-click the icon by the clock** for the transport, Up next, your playlists, shuffle, repeat,
  volume, reverb and night mode, and Lightweight mode itself. On Windows its **Mini player** item
  opens and closes the mini player without ending Lightweight mode.

Show cordinal, the numpad's `.`, Esc in the mini player or Ctrl+Shift+L brings everything back.
cordinal's own keys from other programs (the numpad and Win+Alt+Space) are only taken while
Lightweight mode is on; keys you set yourself from any program are held whenever cordinal runs. If
another program already holds one, cordinal says so and the icon and numpad still work. On Linux the
mode, the tray menu, the picker, the numpad and your keys work the same way, keys from any program
going through the desktop's shortcut settings, which may ask you to allow them the first time.

### Layouts, packs and helpers

**View ▸ Window size** offers Compact, Big mode plus and Big mode, down to the mini player
(Ctrl+Shift+M). Big mode has draggable panel tabs and splitters: F6 focuses a group, arrows select a
tab, Delete closes it, Ctrl+Up/Down moves it between the two areas, Ctrl+Shift+arrow splits it, and
Alt+arrow resizes its split. Reopen panels from **View ▸ Big mode panels**. **Startup defaults**
lets you choose which window size and layout to keep for the next start.

**Packs** bundle customization as a folder or a zipped `.cordpack` file; install and enable them
from **View ▸ Packs**. Enabled packs add columns and named column sets to the track list's header
menu, EQ presets to the equalizer, themes, auto-playlists, shortcuts and layout presets to **View ▸
Customize layout ▸ Presets**, where **Reset the layout** also lives. Layouts include docking on every edge, large covers, Up next, panel
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
**Preferences ▸ Library ▸ Album cover spacing** sets the least space between covers in the Albums and
Artists views.

**Preferences** (Ctrl+,) groups its pages under Sound, Look, Collection and General. Each page is
divided by headings, every setting has its control beside its explanation, and a **Reset** appears
once it differs from the default. **All settings** shows every page in turn, and the search covers
them all.

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

A native Linux build shares the library, decoders, tags and engine with Windows. Its window opens on
Wayland or X11, sound goes to PipeWire (or to PulseAudio where there is no PipeWire), and media keys
and the tray use D-Bus. It has the same features as Windows but is less tested than the Windows
build, and the Windows build remains the reference.

The published program is built on Debian 13 for 64-bit x86 and needs no libraries from the
distribution beyond glibc 2.36 or newer: the C++ runtime is linked in, and everything below is the
program's own code. The window talks to the Wayland compositor or the X server over their protocols,
without libSDL3, libwayland, libX11 or libxcb: xdg-shell windows with server-side decorations,
fractional scaling, cursor shapes, the clipboard, drag and drop and text input on Wayland; MIT-SHM
drawing, XKB keymaps, the clipboard, drag and drop and EWMH hints on X11. Keymaps are read by the
program, without libxkbcommon, and it follows the desktop's scale (`Xft.dpi` and XSETTINGS on X11,
the compositor's preferred scale on Wayland). Drawing is software, so no GPU driver is loaded. File
dialogs use the desktop portal (with a simple built-in one when there is no portal), and links open
through the portal or `xdg-open`. HTTPS is built in (BearSSL, compiled into the program, TLS 1.2) and
trusts the system's root certificates, the `ca-certificates` package on most distributions.

Fonts are found by the program itself: it indexes the TrueType and OpenType fonts in
`/usr/share/fonts`, `/usr/local/share/fonts`, `~/.local/share/fonts`, `~/.fonts` and the directories
fontconfig's configuration names, keeps the index in `$XDG_CACHE_HOME/cordinal` (`~/.cache/cordinal`),
and follows that configuration's aliases the way fontconfig does, without the fontconfig library.
Text is laid out and drawn by the program too, without SDL3_ttf, FreeType or HarfBuzz: TrueType and
CFF outlines, color emoji, kerning, ligatures, Arabic and Hebrew right to left, and Indic and Thai
shaping. Sound is sent by the program's own clients for PipeWire's and PulseAudio's protocols,
without libpipewire or libpulse: it plays on the PipeWire server when one is running and otherwise on
PulseAudio (`CORDINAL_AUDIO=pipewire` or `pulse` picks one). Exclusive output asks PipeWire to run at
the track's rate; PulseAudio always mixes.

The window is drawn to look like the Windows build: the same layout, colours and Preferences window
(page list, search with a scope, switches, drop-downs and Reset buttons), and the transport, speaker
and close icons from Microsoft's open Fluent UI System Icons where Windows uses Segoe MDL2 Assets.
Text is in Segoe UI where it is installed, then Selawik (Microsoft's open stand-in for it), then the
desktop's sans. On Wayland the window offers its icon through `xdg-toplevel-icon` where the
compositor has it; elsewhere the application menu's icon shows, as `install.sh` installs it.

On Debian 12 and newer, Ubuntu 24.04 and newer, Fedora, openSUSE, Arch and their derivatives, and
other 64-bit x86 glibc systems, one command installs it. It installs no libraries; only when the
system has no TrueType or OpenType font at all does it add one from the repositories already
configured on the machine. The download is checked against the release checksum before it is
unpacked.

```sh
curl -fsSL https://github.com/imhavingasuperbadday/cordinal/releases/latest/download/install.sh | sh
```

Run it again to update. Replace the final `sh` with `sh -s -- --uninstall` to remove it; your
library and settings stay. `sh -s -- --dry-run` prints what it would do and stops. Systems older
than glibc 2.36 (Ubuntu 22.04, Debian 11) cannot run the published build, and neither can Alpine's
musl C library. To install a release you already unpacked, run `sh install.sh` in that directory.

## Building

**Windows** needs Visual Studio 2022 (or its Build Tools) with the C++ x64 workload:

```bat
build.bat unpacked
```

That makes a quick development build. Plain `build.bat` also packs the program, downloading the
pinned UPX packer and checking its hash. Releases use cordinal's own packer: `build.bat cdpack`
additionally needs [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)'s `clang++` on `PATH` and
Python 3.14 (see [cdpack/README.md](cdpack/README.md)). Every mode writes `build\cordinal.exe`;
development builds also leave `cordinal.exe` and `cordinal-test.exe` in `build\unpacked\`.

Builds are incremental. Editing one `.cpp` recompiles that file; editing a widely used header
recompiles the files that include it. New `.cpp` files in `src\`, `src\chiptune\`, `src\encode\`,
`src\ipod\`, `src\tests\` and `src\stub\` are picked up automatically; a new source directory needs
an entry in [tools/build.ps1](tools/build.ps1).

**Linux** needs CMake, a C compiler and a C++20 compiler, and no libraries: TLS comes from the
BearSSL source in `src/third_party/bearssl`, and the window, input and drawing from
`src/linux/platform`. `-DCORDINAL_PORTABLE=OFF` links the C++ runtime from the system instead of
into the program. `-DCORDINAL_SELAWIK=ON` compiles in the Selawik font (SIL OFL 1.1, which the FSF
counts as GPL-incompatible, so it is off by default) for the Windows look where Segoe UI and Selawik
are not installed:

```sh
cmake -S . -B build-linux && cmake --build build-linux
```

### Testing

On Windows, `test.bat` builds the development pair, makes missing fixtures when FFmpeg is on
`PATH`, and runs every suite:

```bat
test.bat
```

`test.bat -Headless` runs the offline suites without native UI or an audio device. `test.bat -List`
shows every suite, its requirements and its source file. `test.bat -NoBuild -Suite search,sorting`
runs a focused suite against the existing build, `test.bat -NoBuild -Changed` runs the suites that
cover your uncommitted changes, and `-Seed 42 -Repeat 10` repeats a randomized
run. Results go to a new folder under `build\test-results\`, with a text log, JSON and JUnit
reports, and per-suite scratch files. A failing check prints its file and line on the console and
in the log, and the runner prints a command to reproduce the suite. Read the log before diagnosing a
regression: missing fixtures, setup errors and crashes also return nonzero.

`pwsh -File tools\check-size.ps1` reports the packed and native sizes. The memory-budget suite
runs the real player in everyday states against fixed RAM limits; it needs a window, so `-Headless`
skips it. A packed build over 2,000,000 bytes fails.

On Linux, the build makes two programs: `cordinal` and `cordinal-core-test`. Run
`build-linux/cordinal-core-test` with no arguments for its command list, and
`build-linux/cordinal --check-layout` to compare the window against the shared layout
specification. The release CI runs these checks on Debian 13.

## Contributing and support

- **Bugs:** open an [issue](https://github.com/imhavingasuperbadday/cordinal/issues/new/choose)
  with your cordinal version, your Windows or Linux version and the steps. On Windows, attach a
  crash report from `%LOCALAPPDATA%\cordinal\crashes` if there is one, and a small sample file only
  if you are allowed to share it.
- **Format requests:** use the *Format request* template and attach a short sample file if you can
  share it legally.
- **Features:** open an issue before starting work, so nobody spends a weekend on something that
  does not fit.
- **Security problems:** report them privately, as [SECURITY.md](SECURITY.md) describes.
- **Code, tests and docs:** read [CONTRIBUTING.md](CONTRIBUTING.md) first. It covers the build and
  test loop, the checks a change needs, and the rules for AI-assisted work. Sign the
  [CLA](CLA.md) on your first pull request.

## License

cordinal is free software under the GNU GPL, version 3 or later ([LICENSE](LICENSE)). It is an
original implementation; [THIRD_PARTY.md](THIRD_PARTY.md) lists the outside data and code it
carries.
