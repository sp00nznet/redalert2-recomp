# Red Alert 2 and Yuri's Revenge — Static Recompilation

**[⬇ Download the installer](https://github.com/sp00nznet/redalert2-recomp/releases)**: Windows, Linux and macOS. You need your own copy of the game.

Static recompilation of **Command & Conquer: Red Alert 2** (Westwood Studios,
2000) and its expansion **Yuri's Revenge** (2001) from their shipping Win32
binaries, `game.exe` 1.006 and `gamemd.exe` 1.001, to native C. Both games
build from the same install, each into its own `ra2.exe`, with the same
remaster: the one the first two C&C games got and this engine never did, built
on the game's own code rather than a reimplementation.

Built on the [pcrecomp](https://github.com/sp00nznet/pcrecomp) toolchain.
Tiberian Sun and Firestorm, the same engine's first games, are recompiled the
same way in [tiberiansun-recomp](https://github.com/sp00nznet/tiberiansun-recomp).
This is not [OpenRA](https://www.openra.net/), a separate engine that
reimplements the rules: this runs Westwood's own code, recompiled.

![Main menu](docs/screenshots/main-menu.png)

## What the remaster adds

- **Its own window**: a Direct3D 11 presenter, resizable or borderless
  fullscreen (F11, Alt+Enter), sharp at any size and on high-DPI screens.
- **Scaling** (F12): sharp-bilinear by default, smooth, CRT, nearest, or whole
  multiples only.
- **A settings menu** (F10, or right-click beside the picture): scaling,
  fullscreen, HD vehicles, the bars beside the 4:3 menus, the game's
  resolution and mods, without editing an INI.
- **High resolution and widescreen**: 1280x720 up to 3840x2160, more of the
  battlefield on screen ([hires.md](docs/hires.md)).
- **HD vehicles**: units, their shadows and aircraft drawn at twice (or three
  times) the resolution from the game's own voxel models, at no cost in frame
  time; or *1x smooth*, antialiasing the units at the game's own size
  ([voxels.md](docs/voxels.md)).
- **Modern controls** (F10): left-click to select, right-click to order, and
  Shift-click a build icon to queue five ([modern-controls.md](docs/modern-controls.md)).
- **Mods**: a folder in `mods/`, laid over the game's without changing it,
  switched in game ([mods/README.md](mods/README.md)).
- **LAN multiplayer**, RA2 against RA2, as shipped (IPXEmu).
- **Windows, Linux and macOS**, natively on all three: no Wine, and no
  Visual C++ redistributable to install. It also runs under Wine and CrossOver.

`--classic` gives the original display, and `--original` runs the shipping
machine code for comparison.

## Getting Started

You need **your own copy**: the Steam or EA App (Origin) build of *Command &
Conquer: Red Alert 2 and Yuri's Revenge*, the folder holding `game.exe` and
`gamemd.exe`. Nothing from the game is in this repository or downloaded for
you; everything is built on your machine from your copy.

### The installer (easiest)

1. Download the one for your system from the
   **[latest release](https://github.com/sp00nznet/redalert2-recomp/releases)**:
   `RA2-Recomp-Installer-windows-x64.exe`, `-linux-x64.tar.gz` or
   `-macos-arm64.zip`.
2. Open it. It finds your game in Steam or the EA App, or you point it at the folder.
3. Press **Build my recomp**, and when it's done, **Play**.

The first build takes 30 to 60 minutes and about 6 GB; it installs the tools
it needs (on Windows, Visual Studio's Build Tools, asking for permission
once). The installers aren't signed yet: on Windows, SmartScreen asks first
(**More info**, then **Run anyway**); on a Mac, right-click it and pick **Open**.

### The setup scripts

1. Download this repository (the green **Code** button, then **Download ZIP**)
   and unzip it somewhere with 6 GB free.
2. Windows: double-click **`Setup.cmd`**. Linux and macOS: `./setup.sh`.
3. Play with `Yuri's Revenge (recomp)` or `Red Alert 2 (recomp)` in that folder.

The script asks before installing anything, finds the game, and catalogs,
lifts and builds both games; a rerun skips finished steps. Building by hand,
each platform's details, Wine and the command line are in
**[docs/building.md](docs/building.md)**.

### Linux packages and one-line install

On Linux you can skip the ZIP: the build still runs on your machine,
because it needs your own copy of the game (nothing from the game is
redistributed).

```sh
curl -fsSL https://raw.githubusercontent.com/sp00nznet/redalert2-recomp/main/packaging/install.sh | bash
```

It installs the toolchain (enabling `multilib` on Arch, `i386` on
Debian/Ubuntu where needed), clones to `~/.local/share/redalert2-recomp`,
adds menu entries, then runs the setup. On Arch there is also an AUR
package: `yay -S redalert2-recomp-git` ([AUR](https://aur.archlinux.org/packages/redalert2-recomp-git)),
then run `redalert2-recomp-setup` afterwards.

After the build, play with `redalert2-recomp-play yr` / `ra2`, or the
`Yuri's Revenge (recomp)` / `Red Alert 2 (recomp)` menu entries. Packaging
details (payloads, versions, publishing) are in
**[packaging/README.md](packaging/README.md)**.

In the game: F10 settings, F11 fullscreen, F12 scaling, F9 mods at the main
menu (Linux and macOS). On a Mac hold fn for the F keys, and use Option+Return
for fullscreen.

## Status

**v0.1.0, playable.** Both games lift with 0 errors and play: the
intro, every menu, skirmishes from setup to the score screen, and both
campaigns, 720p to 4K. A scripted playtest suite checks it: 30 of 30 for each
game on Windows, natively on Linux and macOS.

| | Yuri's Revenge | Red Alert 2 |
|---|---|---|
| Functions found | 24,940 (91.8% of the code) | 23,191 |
| Lifted to C | 24,954, 0 errors | 23,201, 0 errors |
| Playtest suite | 30 of 30 | 30 of 30 |

[bringup.md](docs/bringup.md) is the log of each wall and its fix, and
[ROADMAP.md](ROADMAP.md) what comes next.

## Screenshots

Recorded headlessly by the recompiled game. The standout feature is **HD
vehicles** -- the renderer draws voxel units (tanks, aircraft, ships) at two
or three times their size. The same Grizzly tank at 1x, 2x and 3x:

![HD vehicles at 1x, 2x and 3x](docs/screenshots/hd-voxels-scales.png)

An Allied base in a skirmish, HD vehicles drawn at 3x:

![HD vehicles in battle](docs/screenshots/hd-battle.png)

| | |
|---|---|
| ![HD aircraft and ships](docs/screenshots/hd-aircraft.png) | ![HD shadows](docs/screenshots/hd-shadows.png) |
| ![Skirmish](docs/screenshots/skirmish-units.png) | ![Allied campaign](docs/screenshots/campaign-allied.png) |

A few more from the campaign and menus: the [Westwood logo](docs/screenshots/westwood-logo.png),
the [White House](docs/screenshots/white-house.png), the [situation room](docs/screenshots/situation-room.png),
the [Alcatraz briefing](docs/screenshots/alcatraz-briefing.png), [Yuri](docs/screenshots/yuri.png) and
[the attack](docs/screenshots/the-attack.png).

## Documentation

- [docs/building.md](docs/building.md): building by hand, Linux, macOS, Wine, the command line, the lifted C
- [docs/RECON.md](docs/RECON.md): the binaries, the build and the class map
- [docs/host.md](docs/host.md): the host, headless DirectDraw, registration-free COM
- [docs/presenter.md](docs/presenter.md): the presenter window, scaling, the virtual screen
- [docs/hires.md](docs/hires.md): high resolution and widescreen, and the 4K sidebar fix
- [docs/voxels.md](docs/voxels.md): the voxel renderer, mapped, and HD voxels
- [docs/bringup.md](docs/bringup.md): every wall so far and its fix
- [docs/testing.md](docs/testing.md): the playtest suite, scripted input, the `--original` oracle and the LAN game
- [ROADMAP.md](ROADMAP.md), [CHANGELOG.md](CHANGELOG.md), [CONTRIBUTING.md](CONTRIBUTING.md)

## Contributors

See **[CONTRIBUTORS.md](CONTRIBUTORS.md)** for who did what. Thank you, all of
you.

## License

MIT for this repository's own code ([LICENSE](LICENSE)). Red Alert 2 and
Yuri's Revenge are © Electronic Arts; none of their files, and nothing
generated from them, is in this repository ([NOTICE.md](NOTICE.md)).
