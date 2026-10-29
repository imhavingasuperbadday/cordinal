# Contributing to cordinal

Thanks for helping. cordinal is a small, fast, native music player; contributions are welcome
as long as they keep it that way.

## Before you start

- **Bugs:** open an issue with the steps, what you expected, what happened and your
  cordinal version. Attach a crash report from `%LOCALAPPDATA%\cordinal\crashes` if you have one.
- **Formats:** use the *Format request* template and attach a small sample file if you can
  share it legally.
- **Features:** open an issue or discussion first, so nobody spends a weekend on something
  that doesn't fit.

## Building

Requirements and commands are in the [README](README.md#building). `build.bat` is a launcher; the
build definition is [tools/build.ps1](tools/build.ps1), where new sources and compile flags belong.
A pull request must build cleanly with `build.bat`, which also checks the size budget. The
maintainer runs the full test suite before merging.

## Pull requests

- **One change per pull request**, small enough to review in one sitting.
- **Tests with the change.** New behaviour gets a check in `src/tests/`; a fixed bug gets a check
  that fails without the fix.
- **Match the code around you:** naming, comment style, and the README's plain, specific
  writing for anything user-facing.
- **Never put user data at risk.** Anything that writes files, tags or the library must be
  atomic and, where it changes many files, undoable.
- **New dependencies** need a compatible licence; see [THIRD_PARTY.md](THIRD_PARTY.md).
- Update the README when behaviour a user would notice changes.

## AI-assisted contributions

AI-written code is welcome. You are responsible for it as if you had typed it yourself:

- Read and understand every line before you open the pull request.
- It must meet the same test, size and review bar as any other change.
- Don't let a tool copy code from projects with incompatible licences, or from closed-source
  software. Reimplement from public documentation and
  format specifications, and say in the pull request where any ported code came from.
- Say in the pull request description that AI tools were used.

## Contributor licence agreement

Before your first pull request is merged, you sign the [CLA](CLA.md) once. A bot comments with
a link. You keep the copyright to your work; the CLA lets the project ship cordinal under
GPL-3.0-or-later and also in app stores whose terms conflict with the GPL.

## Conduct

Be kind and assume good faith. Harassment, personal attacks and discrimination aren't
tolerated in issues, pull requests or any project space; maintainers may remove content and
block people who don't follow this.
