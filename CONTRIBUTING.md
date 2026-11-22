# Contributing to cordinal

Thanks for wanting to help out. cordinal is a small, fast, native music player — pull requests and bug
reports are welcome as long as they keep it that way. This applies whether you write code by hand or
with an AI assistant.

## Issues & ideas

- **Search first** to see if someone already filed it.
- **Bugs:** include your cordinal version, Windows or Linux version, repro steps, and what happened. On
  Windows, attach any crash log from `%LOCALAPPDATA%\cordinal\crashes`. Only attach audio files you're
  allowed to share.
- **Format requests:** use the *Format request* template and attach a short sample file you can legally
  share.
- **New features:** open an issue first so you don't spend a weekend building something that doesn't fit.
  Small fixes and obvious improvements can go straight to a PR.
- **Security bugs:** don't file a public issue — follow [SECURITY.md](SECURITY.md).

## Building

### Windows

Needs Visual Studio 2022 (or Build Tools) with the C++ x64 workload:

```bat
build.bat unpacked
```

That builds `build\unpacked\cordinal.exe` and `cordinal-test.exe` and copies them into `build\`. Plain
`build.bat` packs with pinned UPX; `build.bat cdpack` builds the release binary with cordinal's own
packer (needs LLVM-MinGW `clang++` on `PATH` and Python 3.14 — read [cdpack/README.md](cdpack/README.md)
before touching the packer).

Builds are incremental. New `.cpp` files in `src\`, `src\chiptune\`, `src\encode\`, `src\ipod\`,
`src\tests\`, and `src\stub\` are picked up automatically. New source folders go in
[tools/build.ps1](tools/build.ps1), new portable core files also go in `CMakeLists.txt`, and any new
source file needs a rule in [tools/test-map.psd1](tools/test-map.psd1).

### Linux

Needs CMake 3.20+, a C compiler, and a C++20 compiler — no external libraries:

```sh
cmake -S . -B build-linux && cmake --build build-linux
```

Produces `build-linux/cordinal` and `build-linux/cordinal-core-test`. See
[README.md](README.md#building) for the portable and Selawik flags.

## Running tests

On Windows, `test.bat` builds the dev binaries, generates missing fixtures (with FFmpeg on `PATH`), and
runs every suite:

```bat
test.bat
```

Fast options while iterating:

```bat
test.bat -NoBuild -Changed                # suites covering your uncommitted changes
test.bat -NoBuild -Suite search,sorting   # specific suites (wildcards work)
test.bat -NoBuild -Suite ui -Seed 42 -Repeat 10
test.bat -Headless                        # skip window and audio-device checks
test.bat -List                            # list suites, requirements, and source files
```

Add `-FailFast` to stop on the first failure, or `-Fixtures <dir>` to reuse an existing fixture folder.
Logs land in `build\test-results\`; failures print `FAIL file:line` and a copy-pasteable reproduce
command. Running `cordinal-test.exe` with no arguments runs nothing and exits 0, so never count that as
a pass.

On Linux:

```sh
build-linux/cordinal-core-test            # run with no args for commands
build-linux/cordinal --check-layout
```

If you touch decoders, tag parsers, or cover-art readers, run the ASan/UBSan tests and fuzzers too (see
[Fuzzing and sanitizers](README.md#fuzzing-and-sanitizers)).

## Engineering rules

Every source file starts with a short header comment explaining what it owns — read it before editing
and keep it accurate.

- **Never risk user files.** File, tag, playlist, and library writes must be atomic (a failed write
  leaves the original byte-identical), and multi-file edits must be undoable. Follow `src/tagwriter.h`
  and `src/streamfile.h`.
- **Keep playback smooth.** Slow work stays off the UI thread. Workers take copies and post results back
  via messages — never touch the live library or window from a worker, and always check for cancellation
  and staleness before applying results.
- **Use the palette and DPI scaling.** Reference colours via `P()`, draw text on filled areas with
  `TextOn`, and scale sizes with `S()`. Themes live in `src/theme_palette.cpp`.
- **Keep Windows and Linux in sync.** Changes to the main window's look update `src/linux/ui*.cpp` in
  the same PR; shared layout metrics and menus live in `src/ui_spec.h`.
- **Don't silently guess preferences.** Ask quietly, accept no, and remember the answer.
- **Zero new runtime dependencies.** cordinal ships as one executable importing only system libraries.
  Any vendored code needs a GPL-3.0-or-later-compatible license and entries in
  [THIRD_PARTY.md](THIRD_PARTY.md) and `src/third_party/README.md`.
- **Watch size and RAM.** Packed builds must stay under 2,500,000 bytes (`pwsh -File tools\check-size.ps1`
  and `build.bat cdpack`). If your change adds caches, buffers, threads, or noticeable code, run the
  `memory-budget` suite — fix regressions instead of bumping limits (and tighten limits if you save RAM).
- **Add a regression check.** Put new checks in the matching `src/tests/selftest_*.cpp` (declared in
  `selftest_common.h`, registered in `selftest_runner.cpp`). Bug fixes need a check that fails without
  the fix. Use the existing helpers (`testlog::Cases`, `testlog::Batch`, `Skip`, `Ready`) — no new test
  frameworks or binaries.
- **Match the surrounding style.** Comments explain *why*, wrapped near 110 columns.

Quick pointers for common changes:
- **Menu action:** ID in `src/app_state.h`, item in `src/app_menus.cpp`, dispatch/state in
  `src/app_commands.cpp`. Settings belong in Preferences; menus hold actions and playback toggles.
- **Preference:** add a Tweaks row in `src/app_tweaks.cpp` and wire load/save with the other settings.
- **Control or dialog:** put it in its owning `src/app_*.cpp`, with shared declarations in its header.

## Pull requests

- **Keep PRs focused.** One coherent change per PR; split refactors from behaviour changes when you can.
- **Fill in the PR template.** Explain what changed, why, and what you actually ran to verify it.
- **Update `README.md`** for user-visible changes. Write plainly for listeners — short, clear, and
  accurate, noting when something is opt-in or platform-specific.
- **Check your diff** (`git diff --check`) and strip stray build outputs, local settings, or unrelated
  formatting changes. Don't force-push over review comments without noting what changed.

## AI-assisted contributions

AI-assisted code, tests, and docs are fine — you own the result as if you typed every line yourself.

- **Disclose it** in the PR description (which tool, what it did).
- **Understand every line** you submit and trim unnecessary refactors, comments, or abstractions.
- **Run everything yourself.** Never paste guessed test output, binary sizes, or benchmark numbers.
- **Check provenance and privacy.** Don't copy incompatibly licensed or closed-source code, and don't
  paste secrets, personal data, or unreleased music into tools that train on inputs.
- **Same bar as hand-written code:** tests, size/RAM budgets, and accurate docs.

## License, CLA & conduct

cordinal is licensed under GNU GPL v3 or later ([LICENSE](LICENSE)). Before your first PR is merged,
sign the [CLA](CLA.md) once (you keep your copyright; the CLA lets the project ship contributions under
GPL-3.0-or-later and other terms, so any third-party code needs compatible permissions).

Be decent to people. Harassment, personal attacks, and discrimination aren't welcome anywhere in the
project.
