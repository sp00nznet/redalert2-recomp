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
