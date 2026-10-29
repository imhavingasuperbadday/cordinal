# Security policy

cordinal reads files from many sources (tags, cover art, playlists, CUE sheets and chiptune
formats), so a malformed file is its main attack surface. Reports are welcome.

## Reporting a vulnerability

Please **don't open a public issue**. Report it privately through GitHub:
**Security ▸ Report a vulnerability** on this repository.

Include the cordinal version, what the problem is, and a sample file or steps that show it.
You'll get a reply within 7 days. A fix ships as soon as it's ready, and you're credited in the
release notes unless you'd rather not be.

## Supported versions

Only the latest release gets security fixes.

## In scope

- Crashes, memory corruption or hangs caused by a file, playlist or folder
- Anything that writes outside the files the user chose, or loses or corrupts user data
- Weaknesses in how the ListenBrainz token is stored or sent
- Problems in the update or packaging process

Bugs in FFmpeg itself, or in Windows components cordinal calls, should go to those projects.
