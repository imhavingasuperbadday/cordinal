# Contributing to cordinal

Thanks for helping. cordinal is a small, fast, native music player, and contributions are welcome as
long as they keep it that way. This guide is for everyone, whether you write the change yourself or
use an AI assistant to help.

## Before you start

- **Search first.** Look through open and closed issues before filing a new one.
- **Bugs:** open an issue with your cordinal version, your Windows or Linux version, the steps,
  what you expected and what happened instead. On Windows, attach a crash report from
  `%LOCALAPPDATA%\cordinal\crashes` if there is one. Attach a sample file only if you are allowed to
  share it.
- **Format requests:** use the *Format request* template. A short sample file helps more than a
  description of the format, but only send one you may legally share.
- **Features:** open an issue before doing the work, so nobody spends a weekend on something that
  does not fit the player. Small improvements can go straight to a pull request if they are clearly
  in scope.
- **Security problems:** do not open a public issue. Follow [SECURITY.md](SECURITY.md).

## What you need

### Windows

Visual Studio 2022, or its Build Tools, with the C++ x64 workload. From the repository root:

```bat
build.bat unpacked
```

That makes `build\unpacked\cordinal.exe` and the test companion, then copies the player to
`build\cordinal.exe`. Plain `build.bat` also packs the player with the pinned stock UPX, and
`build.bat cdpack` makes the release build with cordinal's own packer. The release build needs
LLVM-MinGW's `clang++` on `PATH` and Python 3.14; see [cdpack/README.md](cdpack/README.md) before
changing the packer.

Builds are incremental. Editing one `.cpp` recompiles that file, and editing a widely used header
recompiles only the files that include it. New `.cpp` files in `src\`, `src\chiptune\`,
`src\encode\`, `src\ipod\`, `src\tests\` and `src\stub\` are picked up automatically. A new source
directory needs an entry in [tools/build.ps1](tools/build.ps1).

### Linux

CMake 3.20 or newer, a C compiler and a C++20 compiler. No other libraries are needed: HTTPS comes
from the BearSSL source in the tree, and the window, text, audio and drawing code is part of the
program.

```sh
cmake -S . -B build-linux
cmake --build build-linux
```

That makes `build-linux/cordinal` and `build-linux/cordinal-core-test`. See
[README.md](README.md#building) for the portable and Selawik options.

## The development loop

1. Build for your platform (`build.bat unpacked` on Windows, CMake on Linux).
2. Make the change.
3. Add or update a check that covers it.
4. Run the focused suite.
5. Run the broader checks before opening the pull request.

On Windows, `test.bat` builds the development pair when needed, makes missing fixtures when FFmpeg
is on `PATH`, and runs every suite:

```bat
test.bat
```

Useful forms:

```bat
test.bat -Headless
test.bat -List
test.bat -NoBuild -Suite search,sorting
test.bat -NoBuild -Suite ui -Seed 42 -Repeat 10
```

- `-Headless` runs the offline suites without native controls or an audio device. It still runs
  decoding, tag writing, storage, background workers and offline audio.
- `-List` shows each suite, its requirements and its test source file.
- `-Suite` accepts comma-separated ids or wildcards. Every supplied pattern must match.
- `-Seed` explores other randomized cases, and `-Repeat` starts fresh processes with the same seed.
- `-NoBuild` uses the existing build, and `-Fixtures` reuses a fixture folder made earlier.
- `-FailFast` stops after the first failing suite or run.

Results go to a fresh folder under `build\test-results\`, with a text log, JSON and JUnit reports,
and per-suite scratch files. A failure prints `FAIL <file>:<line>: <message>` on the console and in
the log, and the runner prints a command that reproduces the suite. Read the log before diagnosing a
regression: missing fixtures, setup errors and crashes also return nonzero. `cordinal-test.exe` with
no arguments tests nothing and exits 0, so never report a pass from it.

On Linux, the command-line checks are in `cordinal-core-test`:

```sh
build-linux/cordinal-core-test
build-linux/cordinal --check-layout
```

Run `cordinal-core-test` with no arguments for its command list. The CI Linux job runs the
platform, font, text, audio and layout checks on Debian 13.

## What a change needs

- **A check for new behaviour.** Add it to the matching `src/tests/selftest_*.cpp`, declare it in
  `selftest_common.h`, and add the suite to the registry in `selftest_runner.cpp` if it is new. A
  fixed bug needs a check that fails without the fix.
- **Focused tests first, then broader ones.** A change to a shared system needs the suites that use
  it, not only the one that first failed.
- **No new test framework and no second test binary.** Use the existing checks, `testlog::Cases` for
  many cases of one behaviour, `testlog::Batch` for related assertions about one operation, `Skip`
  for unavailable capabilities and `Ready` for setup.
- **A size check when code grows.** A packed build must not exceed 2,000,000 bytes. Run
  `pwsh -File tools\check-size.ps1` and, for a release build, `build.bat cdpack`.
- **A memory check when the change adds data, caches, threads, buffers or code.** Run the
  `memory-budget` suite, which starts the real player in everyday states and compares its private
  working set and image size against fixed limits. Fix growth instead of raising a limit. If a
  saving makes the player smaller, lower the limit in the same change.
- **No new runtime dependency.** The player ships as one executable with system libraries only.
  Anything new must have a GPL-3.0-or-later-compatible licence and be recorded in
  [THIRD_PARTY.md](THIRD_PARTY.md) and `src/third_party/README.md`.
- **README updates for visible changes.** If a user would notice it, document it in plain words and
  keep the existing structure and tone.

## Code and UI rules

Every source file opens with a comment saying what it is responsible for. Read it before changing
the file, and keep it true.

- **Protect user data.** File, tag, playlist and library writes are atomic; a failed write leaves the
  original byte-identical. Edits across many files are undoable. Follow `src/tagwriter.h` and
  `src/streamfile.h`.
- **Keep playback responsive.** Slow work runs off the UI thread. Workers receive copies and report
  back by message; they do not touch the live library or the window. Check cancellation and whether
  a result still applies before using it.
- **Use the theme palette and DPI scaling.** Name colours through `P()`, use `TextOn` for text on a
  fill, and use `S()` for measurements. Themes belong in `src/theme_palette.cpp`.
- **Keep the platforms in step.** A change to the main window's appearance updates the Linux window
  in `src/linux/ui*.cpp` in the same change. Shared measurements and menu bar text live in
  `src/ui_spec.h`.
- **Do not save a preference the user did not confirm.** Ask at a quiet moment, accept no, and
  remember the answer.
- **Match the code around you.** Use the local naming, comment and formatting style. Comments should
  explain why, in plain and specific words, wrapped near 110 columns.

For a menu action, add the id in `src/app_state.h`, the item in `src/app_menus.cpp`, and the dispatch
and enabled or checked state in `src/app_commands.cpp`. Settings belong in Preferences; menus hold
actions and playback-time toggles. For a preference, add a registry row in `src/app_tweaks.cpp`
with its key, default and help, and load and save it with the other preferences. A new control or
dialog belongs in its owning `src/app_*.cpp`, with shared declarations in the matching header.

## Pull requests

- **One coherent change per pull request**, small enough to review in one sitting. Separate a
  refactor from a behaviour change unless they cannot be separated.
- **Explain what changed and why**, and say how you tested it. The pull request template has the
  checklist; fill it in rather than deleting it.
- **Run `git diff --check`** and review your own diff before opening the pull request. Remove build
  outputs, local settings, crash reports, generated fixtures and unrelated formatting changes.
- **Use a clear commit message.** A short imperative summary, then a paragraph about the effect and
  the checks if the summary is not enough.
- **Do not force-push over review comments** without saying what changed. Reviewers read commits,
  not only the final tree.
- **Be patient and responsive.** The maintainer runs the full suite before merging, and CI runs the
  Windows and Linux jobs. A small, tested change is reviewed faster than a large one.

## AI-assisted contributions

AI-written code, tests and docs are welcome. You are responsible for the result as if you had typed
it yourself.

- **Say how AI was used** in the pull request description. Name the tools and what they did, such as
  drafting code, writing tests, reviewing a diff or editing documentation.
- **Read and understand every line.** You should be able to explain why each part is there. Do not
  submit code you cannot debug.
- **Run the commands yourself.** Do not let a tool invent test results, file sizes, version numbers,
  benchmark numbers or error messages. Paste output from a run you actually did.
- **Keep the diff small.** AI tools tend to add unnecessary refactors, dependencies, comments,
  abstractions and tests. Remove anything the change does not need.
- **Check licences and provenance.** Do not let a tool copy code from projects with incompatible
  licences or from closed-source software. Reimplement from public documentation and format
  specifications, and say where any ported code came from.
- **Protect private material.** Do not paste secrets, API keys, personal data, unreleased music or
  private issue content into a tool that keeps or trains on it.
- **Do not trust generated docs.** Check every path, command, menu name and link against this
  checkout. If a command has not been run, say so.
- **The same bar applies.** Generated code needs the same tests, size and memory checks, review and
  licence checks as anything else. A tool's confidence is not evidence.

## Documentation style

The README is the user manual. Write for someone who wants to play music, not for someone reading
the source.

- Use plain, specific words and sentence case for labels and headings.
- Name the result, not the mechanism. Prefer the menu path a user can follow.
- Do not use em dashes. Use a comma, a colon, parentheses or a full stop instead.
- Keep commands copy-pasteable and paths exact.
- Do not document behaviour you have not verified. Say clearly when something is optional, off by
  default or platform-specific.
- Keep old release notes and screenshots out of the README. User-facing changes belong in the README;
  the notes for a release belong with that release.

## Licensing and the CLA

cordinal is licensed under GPL-3.0-or-later; see [LICENSE](LICENSE). Before your first pull request
is merged, sign the [CLA](CLA.md) once using the instructions there. You keep any copyright you own
in your work; the CLA lets the project ship your contribution under GPL-3.0-or-later and also under
other terms. Third-party material must have the necessary permissions for those terms; GPL
compatibility alone is not enough.

## Conduct

Be kind and assume good faith. Harassment, personal attacks and discrimination are not tolerated in
issues, pull requests or any project space. Maintainers may remove content and block people who do
not follow this.
