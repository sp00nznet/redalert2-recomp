# Red Alert 2 and Yuri's Revenge — Static Recompilation

Static recompilation of **Command & Conquer: Red Alert 2** (Westwood Studios,
2000) and its expansion **Yuri's Revenge** (2001) from their shipping Win32
binaries, `game.exe` 1.006 and `gamemd.exe` 1.001, to native C. Both games
build from the same install, each into its own `ra2.exe`, with the same
remaster. The goal past running them is the remaster the first two C&C games
got and this engine never did: higher resolutions, proper scaling and a modern
presentation layer, built on the game's own code rather than a reimplementation.

Built on the [pcrecomp](https://github.com/sp00nznet/pcrecomp) toolchain and
following its shared house style (layout, CLI, harness, headless mode). Tiberian
Dawn and Red Alert have released source and are ported from it; this engine has
none, so it is recompiled. Tiberian Sun and Firestorm, the same engine's first
games, have a repo of their own.

This is not [OpenRA](https://www.openra.net/). OpenRA is a separate engine
that loads the original assets and reimplements the rules; this project runs
Westwood's own code, recompiled.

## What the remaster adds

Everything here is built around the game's own code, recompiled; `--classic`
gives the original display, and `--original` runs the shipping machine code
for comparison.

- **Its own window.** The game draws into a Direct3D 11 presenter instead of
  taking over the screen: a resizable window or borderless fullscreen (F11,
  Alt+Enter), sharp at any size and crisp on high-DPI screens.
- **Scaling** (F12): sharp-bilinear (the default: whole pixels stay crisp
  and only their edges blend), smooth, CRT, nearest, or whole multiples only.
- **A settings menu** (F10, or right-click beside the picture): scaling,
  fullscreen, HD vehicles, the bars beside the 4:3 menus, and the game's
  resolution, without editing an INI. The presenter remembers its settings
  and the window's place in `build\ra2.ini`.
- **High resolution and widescreen**: 1280x720 up to 3840x2160 in game, more
  of the battlefield on screen. 4K needed a fix to RA2's own sidebar, which
  overran its button array (docs/hires.md).
- **Framed menus**: the 800x600 menus in a widescreen window sit on a soft,
  dark blur of themselves instead of black bars (or black, if you prefer).
- **HD vehicles**: units, their shadows and aircraft are drawn at twice the
  resolution of the rest of the picture, from the game's own voxel models:
  sharper barrels, hulls and silhouettes, and solid shadows with smooth
  edges where the original has a 1x stipple. It costs no frame time
  (docs/voxels.md). Voxel debris is opt-in until a test exercises it.
- **LAN multiplayer**: RA2 against RA2 between two PCs, as shipped (IPXEmu),
  with a script for each side to play a match start unattended
  (docs/testing.md).
- **Windows, Linux and macOS**: on Windows the C runtime is linked in, so
  there is no Visual C++ redistributable to install. On Linux the game also
  runs **natively, with no Wine**: the same lifted C on pcrecomp's own
  implementation of the Windows API, with SDL2 for the window and sound and
  ffmpeg for the Bink movies ([Linux, native](#linux-native)). On macOS (and
  Linux) the exe is cross-built with clang-cl and played under CrossOver or
  Wine ([macOS and Linux, under Wine](#macos-and-linux-under-wine)). `--mute`
  for silent runs.

## Status: **v0.1.0-dev, playable.** The whole game lifts with 0 errors and plays: every main-menu screen, skirmishes from setup to the score screen, and both campaigns, in the presenter or headless, checked by a scripted test suite.

| Stage | State |
|---|---|
| P0: pick the build | the Steam build of *The Ultimate Collection*: `gamemd.exe`, 2001-10-31, no DRM, launcher check already patched out ([RECON.md](docs/RECON.md)) |
| RTTI class recovery | 954 classes, 1,214 vtables, 6,665 virtual methods |
| Function catalog (`disasm32`) | 24,940 functions, 91.8% of `.text`, 15 minutes |
| Lift (`run_lift.py --all`) | 24,954 functions, 5.8M lines of C, **0 lift errors** |
| Host (`build/ra2.exe`, 32-bit, pcrecomp `native32`) | plays: the intro, every menu, skirmishes and both campaigns, 720p to 4K, in its own Direct3D 11 presenter or headless ([bringup.md](docs/bringup.md), [presenter.md](docs/presenter.md)) |
| Playtest suite (`tools/playtest.py`) | **29 of 29 passing**, with HD vehicles off and on: every menu screen, every way back, a skirmish start to score screen, orders by mouse (select, deploy, force-fire), the Allied and Soviet campaigns, 720p to 4K: scripted by button name, run in parallel, and `--original` runs the same script on the shipping code to tell lift bugs from host bugs ([testing.md](docs/testing.md)) |
| Presenter (the default display) | the game in its own Direct3D 11 window: five scalings (F12), borderless fullscreen (F11), the settings menu (F10), blurred bars, settings remembered; `--classic` is the original DirectDraw ([presenter.md](docs/presenter.md)) |
| HD vehicles | units, their shadows and aircraft at 2x, from four half-pixel-offset renders of each model, remembered by their 1x pixels; no change in frame time; voxel debris opt-in ([voxels.md](docs/voxels.md)) |
| High resolution / widescreen | 720p, 1080p, 1440p and 4K in game, skirmish and campaign, picked from the presenter's settings menu (F10) or `RA2MD.INI`; 4K needed a fix to RA2's own sidebar ([hires.md](docs/hires.md)) |
| Headless mode | `--headless --record out.mp4 --frames N`: hidden window, no mode change, the primary surface recorded to ffmpeg ([host.md](docs/host.md)) |
| Multiplayer | RA2 against RA2 over the LAN between two PCs, scripted on both sides into the game ([testing.md](docs/testing.md)) |
| Native Linux (`build-linux/ra2`, pcrecomp `win32hle`) | both games as Linux programs, no Wine: the Bink movies, every menu, skirmishes, both campaigns, saves, HD vehicles; the playtest suite in Docker ([Linux, native](#linux-native)) |
| Red Alert 2 (`game.exe`) | a second target from the same install: 23,201 functions, 0 lift errors, the suite at 30 of 30, 720p to 4K, HD vehicles ([bringup.md](docs/bringup.md), section 13) |
| Compilers | MSVC (x86); clang-cl (x86) with pcrecomp #47 |
| Conformance harness | `tools/conformance.py`: **8/8** boot milestones up to the main menu, lift 0 errors, against `conformance.json`; fails on regression |

[bringup.md](docs/bringup.md) is the log of each wall and its fix. Five of them
were toolkit bugs, fixed in pcrecomp rather than here: catalog entries in
alignment padding (66 functions hidden), MASM's alignment fillers (every
vehicle invisible), a mid-body fall-through, loops that never let the sound
thread run, and a `push; jmp` read as a call.

## Screenshots

Rendered by the recompiled game and recorded headlessly (`--headless
--record`) over RDP, with nothing on any screen. The main menu, then the
Westwood logo and the Yuri's Revenge intro, decoded by Bink into the game's
own primary surface.

![Main menu](docs/screenshots/main-menu.png)

HD vehicles, 1x on the left and 2x on the right (zoomed): Grizzly tanks and the
MCV, a Night Hawk and a destroyer, and a unit's shadow.

![HD vehicles](docs/screenshots/hd-voxels.png)

| | |
|---|---|
| ![HD aircraft and ships](docs/screenshots/hd-aircraft.png) | ![HD shadows](docs/screenshots/hd-shadows.png) |

In game, from the playtest suite: the Allied campaign's first mission, the
Soviet one, a skirmish with its MCV and Grizzlies, its setup screen and the
score screen at the end.

| | |
|---|---|
| ![Allied campaign](docs/screenshots/campaign-allied.png) | ![Soviet campaign](docs/screenshots/campaign-soviet.png) |
| ![Skirmish](docs/screenshots/skirmish-units.png) | ![Skirmish setup](docs/screenshots/skirmish-setup.png) |
| ![Skirmish score](docs/screenshots/skirmish-score.png) | |

| | | |
|---|---|---|
| ![Westwood logo](docs/screenshots/westwood-logo.png) | ![White House](docs/screenshots/white-house.png) | ![Situation room](docs/screenshots/situation-room.png) |
| ![Alcatraz briefing](docs/screenshots/alcatraz-briefing.png) | ![Yuri](docs/screenshots/yuri.png) | ![The attack](docs/screenshots/the-attack.png) |

## Getting Started

You need **your own copy of Red Alert 2 and Yuri's Revenge**: the Steam build
of *Command & Conquer: Red Alert 2 and Yuri's Revenge* (the folder holding
`game.exe` and `gamemd.exe`). One install has both games, and both are built:

| Game | Exe | Builds into | Play with |
|---|---|---|---|
| Red Alert 2 | `game.exe` | `build-game\ra2.exe` | `Red Alert 2 (recomp).cmd` |
| Yuri's Revenge | `gamemd.exe` | `build\ra2.exe` | `Yuri's Revenge (recomp).cmd` |

Nothing from the game is in this repository and nothing is
downloaded for you. The lifted C is generated on your machine from your copy
and is never distributed. On Linux, see [Linux, native](#linux-native); on
macOS, [macOS and Linux, under Wine](#macos-and-linux-under-wine).

### Quick start

1. Download this repository (the green **Code** button, then **Download ZIP**)
   and unzip it somewhere with 6 GB free.
2. Double-click **`Setup.cmd`**.

It checks for Python 3.10+, the `pefile` and `capstone` packages, the pcrecomp
toolkit, Visual Studio 2022 with the C++ x86 tools, CMake and Ninja, and
**asks** before installing anything. It finds the game in your Steam library or
asks for the folder and copies it into `game\`. Then, for each game, Yuri's
Revenge first and then Red Alert 2, it builds the function catalog, lifts and
builds. A rerun skips finished steps. If it stops, it says why in one
sentence; the details are in `setup.log`.

Setup.cmd runs exactly the commands in *Step by step*, and has been run end
to end from a clean folder (the ZIP download).

It ends with `Red Alert 2 (recomp).cmd` and `Yuri's Revenge (recomp).cmd` in
this folder: double-click either to play. F10 opens the settings.

Only one of them? `Setup.cmd -Games ra2` or `Setup.cmd -Games yr` (from a
terminal in this folder) builds just that game, in about half the time.

### Step by step

Prerequisites: Windows 10/11, **Python 3.10+** (`py -3 --version`), **git**,
**Visual Studio 2022** (any edition, or the Build Tools) with *Desktop
development with C++* including the x86 tools, **CMake 3.20+** and **Ninja**,
and the pcrecomp toolkit cloned **beside** this repository as `tools`:

```
some-folder\
  tools\        <- git clone https://github.com/sp00nznet/pcrecomp tools
  ra2\          <- this repository
```

1. Python packages:
   ```
   py -3 -m pip install --user pefile capstone
   ```
2. Copy your install into `game\` (about 1.9 GB):
   ```
   robocopy "C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Red Alert II" game /E
   ```
3. Headers, imports and C++ classes (seconds):
   ```
   py -3 ..\tools\tools\pe\pe_analyze.py game\gamemd.exe --json work\pe_analysis.json
   py -3 ..\tools\tools\cpp\rtti.py game\gamemd.exe -o work\rtti.json --seeds work\rtti_seeds.json
   ```
   Expected from `rtti.py`: `classes : 954` and `virtual methods : 6,665`.
4. The function catalog (about 15 minutes):
   ```
   py -3 ..\tools\tools\disasm\disasm32.py game\gamemd.exe -o work\functions.json --seed-functions work\rtti_seeds.json
   ```
   Expected: `Functions: 24940` and `Byte coverage: ... (91.8% of code range)`.
5. Lift (5 to 15 minutes):
   ```
   py -3 run_lift.py --all
   ```
   Expected: `lifted 24954   not-lifted stubs 0   errors 0`.
6. Build (from a plain terminal; `build.cmd` sets up the x86 compiler itself):
   ```
   build.cmd
   ```
7. Play:
   ```
   build\ra2.exe --run
   ```

Steps 3 to 7 are Yuri's Revenge. Red Alert 2 is the same from step 3, with
`game.exe`, `work\game\` and `build-game\`:

```
py -3 ..\tools\tools\pe\pe_analyze.py game\game.exe --json work\game\pe_analysis.json
py -3 ..\tools\tools\cpp\rtti.py game\game.exe -o work\game\rtti.json --seeds work\game\rtti_seeds.json
py -3 ..\tools\tools\disasm\disasm32.py game\game.exe -o work\game\functions.json --seed-functions work\game\rtti_seeds.json
py -3 run_lift.py --all --target game
set BUILD_DIR=build-game
set CMAKE_ARGS=-DRA2_TARGET=game
build.cmd
build-game\ra2.exe --run
```

Expected: `Functions: 23191` and `lifted 23201   not-lifted stubs 0   errors 0`.
Everything in *What the remaster adds* works on both; the playtest suite and
conformance take `RA2_TARGET=game` (docs/testing.md).

The usual trip-ups: `python` opening the Microsoft Store (that is Windows' alias;
use `py -3`), and a PATH change that needs a new terminal window.

### Linux, native

`build-linux/ra2` (Yuri's Revenge) and `build-linux-game/ra2` (Red Alert 2)
are 32-bit Linux programs: the same lifted C as the Windows build, on
pcrecomp's `runtime/win32hle`, which answers every Windows call the game
makes itself (files, windows and dialogs, DirectDraw and DirectSound on SDL2,
the Bink movies on ffmpeg, Winsock, the registry, COM and save-game storage).
No Wine and no Windows DLLs; the game folder is only data.

On Debian or Ubuntu (`setup.sh` names the Fedora and Arch packages):

```
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install gcc-multilib cmake ninja-build pkg-config python3-pefile python3-capstone fonts-liberation \
                 libsdl2-dev:i386 libsdl2-ttf-dev:i386 libavformat-dev:i386 libavcodec-dev:i386 libswresample-dev:i386 libavutil-dev:i386
./setup.sh
```

`setup.sh` links `game/` to your install (it looks in your Steam libraries),
catalogs, lifts and builds both games, and leaves `Yuri's Revenge (recomp).sh`
and `Red Alert 2 (recomp).sh` (`./setup.sh --wine` builds the Windows exes
for Wine instead). By hand, it is *Step by step* with `python3` for `py -3`
and `./build-linux.sh` for `build.cmd` (`BUILD_DIR=build-linux-game
CMAKE_ARGS=-DRA2_TARGET=game` for Red Alert 2), then `build-linux/ra2 --run`.
It needs pcrecomp with win32hle's DirectDraw and Bink (pcrecomp #62).

The window scales like the Windows presenter: F12 cycles sharp, smooth, CRT,
nearest and integer scaling, F11 (or Alt+Enter) is fullscreen, and `ra2.ini`
beside the program remembers them (`--scale` on the command line).
`--headless`, `--record`, `--mute`, `--hd-voxels`, `--seed` and the scripted
input are the Windows host's; `tools/playtest.py` runs the native build when
it is there. LAN games speak IPXEmu's protocol (IPX over UDP), the one the
Windows build uses through the game folder's `wsock32.dll`.

On Debian 13, headless in Docker, the playtest suite passes 30 of 30 for
each game, Yuri's Revenge and Red Alert 2: every menu, skirmishes to 4K,
both campaigns, the loop to the score screen. Yuri's Revenge also plays its
movies and menus in a window on an Xfce desktop. Not on this host:
`--original` (it runs the shipping machine code on Windows), the F10
settings menu, and a LAN game against a Windows player (untried).

### macOS and Linux, under Wine

The same `ra2.exe`, cross-compiled with clang-cl and played under Wine:
CrossOver on a Mac, `wine` on Linux. No Visual Studio and no copy of the
game: `game/` is a link to your install. On a Mac, install Steam for Windows
in a CrossOver bottle and Red Alert 2 from your library in it; on Linux,
install it from Steam (which runs it with Proton) or have the folder anywhere.
Then:

```
./setup.sh
```

On a Mac it checks for and offers to install Homebrew's `llvm`, `lld`,
`cmake`, `ninja`, `xwin` and `uv`. On Linux it lists what your package manager
should install (clang-cl, lld, llvm, cmake, ninja, wine and Python 3) and
offers to download `xwin`'s release binary. Either way it asks before xwin
downloads the x86 MSVC C runtime and Windows SDK from Microsoft (about 1 GB,
under Microsoft's licence, into `~/.xwin`); finds `pefile` and `capstone` or
puts them in a `.venv`; clones pcrecomp beside this folder as `pcrecomp` (or
uses `../tools`, or `PCRECOMP`); finds the game in your CrossOver bottles or
Steam libraries; and then catalogs, lifts and builds each game as `Setup.cmd`
does. The catalog is about 15 minutes a game, the lift a minute or two, the
build 10 to 20. It leaves `Yuri's Revenge (recomp)` and `Red Alert 2 (recomp)`
launchers here (`.command` on a Mac, `.sh` on Linux).

By hand, the steps are *Step by step*'s with `python3` (or `.venv/bin/python`)
for `py -3` and `./build.sh` for `build.cmd`; `./play.sh` (Yuri's Revenge) or
`./play.sh ra2` runs a build under Wine, with any host flags after it (on a
Mac, `CX_BOTTLE` picks the bottle, default `Steam`). `tools/playtest.py` and
`tools/conformance.py` run the host through Wine off Windows (`RA2_WINE` for
another launcher).

It needs pcrecomp's native32 with Wine support (pcrecomp #55): DEP turned on
at start (Wine otherwise answers the first fetch from the guest's code by
making it executable, and runs the shipping machine code) and a fetch fault
that Wine under Rosetta reports as a read accepted as a callback. The network
games need the IPXEmu `wsock32.dll` the Steam release has in the game folder,
and `play.sh` and the playtests tell Wine to load it
(`WINEDLLOVERRIDES=wsock32=n,b`); an install that CnCNet has updated may no
longer have it, and then the game cannot create its IPX socket (10047).

| Where | Yuri's Revenge | Red Alert 2 |
|---|---|---|
| Linux, Wine 10.0 (Debian 13, x86-64) | 30 of 30 | 29 of 30 |
| macOS, CrossOver (Apple Silicon) | 27 of 30 | 27 of 30 |

The Mac runs predate the IPXEmu override, and the three cases they missed
were the network ones. Red Alert 2's `skirmish-build` is the one miss on Linux: its
skirmish has no fixed seed, and the AI often wins it in the 230 seconds, on
Windows as well (a flaky case, not a Wine one). One Red Alert 2 `skirmish-loop` run on the Mac hung
once in `Theme::Stop` after the defeat and passed when run again.

Not yet under Wine: recordings (`--record` starts `ffmpeg` through Wine's
`cmd`, which cannot run a Mac or Linux program; the frame checksums the tests
count still come out), and `--original` is untested.

## Usage

`build\ra2.exe` is Yuri's Revenge and `build-game\ra2.exe` Red Alert 2; they
take the same flags.

```
build\ra2.exe                                   # dry run: map and bind, print the entry point
build\ra2.exe --run                             # play: our own window; settings F10, scaling F12, fullscreen F11
build\ra2.exe --run --fullscreen --scale crt     # borderless fullscreen, CRT look
build\ra2.exe --run --classic                   # the original exclusive-fullscreen DirectDraw
build\ra2.exe --headless --run --watchdog 60    # no window, no mode change, stop after 60 s
build\ra2.exe --headless --run --record out.mp4 --frames 300
build\ra2.exe --headless --run --hd-voxels-dump hd    # HD vehicles headless: 1x/2x renders and 2x frames into hd\
py -3 tools\conformance.py                      # boot milestones + lift health vs the baseline
py -3 tools\playtest.py --jobs 3                 # every menu and game mode, scripted (docs\testing.md)
py -3 tools\playtest.py campaign-allied --original   # the same script on the shipping code
```

Scripted input (headless or in the presenter): `--press DLG:CTRL@s` presses a menu button by
dialog and control ID once that screen is open, `--select DLG:CTRL=N@s`,
`--waitlog TEXT@s`, `--key`, `--move`, `--click` (with Ctrl, Shift or Alt held:
`--click c+x,y@s` is force-fire), `--drag x1,y1,x2,y2@s` (band selection),
`--wait`. `--original` runs the shipping machine code under the same host.

`--mute` keeps a run silent (the playtest suite and the conformance harness use it);
`--args FILE` reads more arguments from a file (a script of presses, as the
LAN game's sides use: `tools\lan\*.args`). A script runs headless or in the
presenter, not with `--classic`.

Environment: `RA2_EXE` (another build to test: the playtest runner and the
conformance harness use it in place of `build\ra2.exe`), `RA2_HOST_ARGS` (extra host flags for every playtest case),
`RA2_FRAME_STATS=1` (time between frames), `RA2_HD_VOXEL_ANIMS=1` (HD voxel
debris, opt-in).

Diagnostics: `--debuglog` (the game's own debug log), `--native-trace`
(every call into Windows), `--callbacks`,
`--probe VA`, and with a `-DRA2_TRACE=ON` build `--calltrace FILE` and the
other pcrecomp trace options (`build\ra2.exe --help`).

### Mods

A mod is a folder of the game's own kind of files (rules, art, string tables,
MIX files, maps) in `mods/yr/<name>/` (Yuri's Revenge) or `mods/ra2/<name>/` (Red Alert 2): see [mods/README.md](mods/README.md). The game's
folder is never changed. The mod's folder is laid over it, so the game reads
the mod's file where it has one and its own where it does not, and what it
writes while a mod is on (settings, saves) goes into the mod's folder.
`--mod NAME` plays one (`--mod none`, none); on Windows the settings menu
(F10) lists them, on Linux F9 Tried: R.O.T.K. (rules, art, AI and a Chinese string table, on NPatch; its `NPatch.mix` renamed `expandmd90.mix`), a rules-only mod, and a pack of Red Alert 2 maps. steps through them, and either restarts the
game with the one chosen and remembers it. Mods built on DLLs that patch
`gamemd.exe`'s machine code (Ares, Phobos and the like) cannot work on a recompiled
game. at the main menu

### Reading the lifted C

The lift (`src/recomp/gen`, made on your machine, never committed) names what
the binary itself names (pcrecomp's `tools/lift/name_lift.py`): virtual
methods by their class and vtable slot from RTTI (`UnitClass__virtual_42`,
COM's by name: `OverlayClass__Load`), constructors, destructors and
`operator_delete` by their shape, and functions that print their own name in
a debug message by it. Each function has a header saying how it was named,
its `this` class, the strings it uses, the Windows calls it makes and how many
places call it, and a constant that is a string's or a vtable's address has a
comment. A function with no such evidence keeps its address name
(`sub_` and its address) and still gets the header. The address is in each header and
in its `RECOMP_ENTER`, so a crash report's address finds the function.

## Building from source

Steps 5 and 6 above. `PCRECOMP` (environment, for `run_lift.py`) and
`-DPCRECOMP=` (CMake) point at a toolkit checkout other than `..\tools`; the
lifter and the runtime must come from the same tree.

It builds from pcrecomp `main`: the four toolkit fixes found here (#41
mid-body fall-through, #42 lifted loops yield the machine, #43 push/jmp
inside a body, #44 MASM alignment fillers in the catalog) are merged. A
clang-cl build also needs #47 (native32's bridge under clang-cl).

Contributions: [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

- [docs/RECON.md](docs/RECON.md): the binaries, the build and the class map
- [docs/host.md](docs/host.md): the host, headless DirectDraw, registration-free COM
- [docs/presenter.md](docs/presenter.md): the presenter window, scaling, the virtual screen
- [docs/hires.md](docs/hires.md): high resolution and widescreen, and the 4K sidebar fix
- [docs/voxels.md](docs/voxels.md): the voxel renderer, mapped, and HD voxels
- [docs/bringup.md](docs/bringup.md): every wall so far and its fix
- [docs/testing.md](docs/testing.md): the playtest suite, scripted input, the `--original` oracle and the LAN game
- [ROADMAP.md](ROADMAP.md), [CHANGELOG.md](CHANGELOG.md)

## Contributors

See **[CONTRIBUTORS.md](CONTRIBUTORS.md)** for who did what. Thank you, all of
you.

## License

MIT for this repository's own code ([LICENSE](LICENSE)). Red Alert 2 and
Yuri's Revenge are © Electronic Arts; none of their files, and nothing
generated from them, is in this repository.
