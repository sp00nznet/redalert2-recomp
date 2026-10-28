# Contributors

Thank you to everyone who has contributed code, fixes, testing, or a hard-won
debugging insight. This file is the canonical record of who did what; the
CHANGELOG tells the story release by release, but credit lives here.

If you've contributed and aren't listed (or a line is wrong), open a PR against
this file. We want every name right.

---

## Maintainer

### Ned Heller ([@sp00nznet](https://github.com/sp00nznet))
Project creator and maintainer. The recompiled host, its presenter, HD voxels,
the playtest suite, and the pcrecomp toolkit it is built with.

---

## Contributors

### Chris Pressland ([@cpressland](https://github.com/cpressland))
**The first build off Windows.** Took the game to an Apple Silicon Mac and made
it play there:

- **Cross-compiling on macOS** (#1): a clang-cl and lld-link toolchain file
  against xwin's MSVC CRT and Windows SDK, `build.sh`, `play.sh`, and a
  `setup.sh` that finds the game in a CrossOver bottle and runs the whole
  pipeline. The Linux build is the same design, extended.
- **Playtests under Wine** (#1): `tools/playtest.py` and `tools/conformance.py`
  run the host through Wine with `Z:` paths, and `skirmish-build` sets its own
  640x480, after a CnCNet INI's 3440x1440 made every sidebar click miss.
- **Callbacks under Wine** (sp00nznet/pcrecomp#55): found that Wine leaves DEP
  off, so the game's dialog procedures quietly ran its original machine code
  instead of the lifted C, and that Rosetta reports an instruction fetch as a
  read. Without both fixes nothing runs under Wine, on a Mac or on Linux.

### [@andersbusk](https://github.com/andersbusk)
**Diagnosed and patched, each with its own tests:**

- **The battle hang** (#20): sampled the stuck thread down to the line
  clipper at 0x00775620, read its locals out of the hung process, and wrote
  the round cap for both games.
- **21:9 sizes and centred menus** (#21): the ultrawide resolutions, and the
  menu band cut that sliced Red Alert 2's main menus at widescreen sizes.
- **The presenter's timer** (#22): measured `GetTickCount`'s 15-16 ms steps
  holding menus to 21 pictures a second, not 30.

### [@hauxir](https://github.com/hauxir)
**Brought up a retail (non-Steam) copy on an Apple Silicon Mac:**

- **The launcher check** (#32): found that a retail exe of the Steam build
  still bails with "Launcher not running", and stubbed the check and the
  notify in the lift for both games.
- **The 3x HD startup crash** (#32): traced it with lldb to the presenter's
  2x-sized fallback buffer and kept the fallback to 2x.
- **Quit that leaves** (#33): closing the window or Cmd+Q left the native
  game running with no window; the host now exits if the game doesn't.
- **A Mac app** (#35): `tools/package_macos.py` packs the native host into a
  double-clickable `.app` with its libraries inside, and `tools/deps-macos.sh`
  builds those libraries for macOS 11, so the app runs on any Apple Silicon Mac.
