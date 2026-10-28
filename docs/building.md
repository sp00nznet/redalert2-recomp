# Building, platforms and the command line

The [installer](https://github.com/sp00nznet/redalert2-recomp/releases) and
the setup scripts do all of this for you. This is what they do, by hand, and
the rest of the details: each platform, Wine, the command line and the lifted
C.

- [The setup scripts](#the-setup-scripts)
- [Step by step (Windows)](#step-by-step-windows)
- [Linux, native](#linux-native)
- [macOS, native](#macos-native)
- [macOS and Linux, under Wine](#macos-and-linux-under-wine)
- [The command line](#the-command-line)
- [Reading the lifted C](#reading-the-lifted-c)

## The setup scripts

`Setup.cmd` (Windows) checks for Python 3.10+, the `pefile` and `capstone`
packages, the pcrecomp toolkit, Visual Studio 2022 with the C++ x86 tools,
CMake and Ninja, and **asks** before installing anything (`-Yes` installs
without asking). It finds the game in your Steam or EA App library or asks for
the folder (`-Game DIR` gives it) and copies it into `game\`. Then, for each
game, Yuri's Revenge first and then Red Alert 2, it builds the function
catalog, lifts and builds. A rerun skips finished steps. If it stops, it says
why in one sentence; the details are in `setup.log`. `-Games ra2` or
`-Games yr` builds just that game, in about half the time.

`setup.sh` is the same on Linux and macOS (`--yes`, `--game-dir DIR`, `yr` or
`ra2`), building natively by default and for Wine with `--wine`.

To update, get this repository again (or `git pull`), `git pull` the pcrecomp
toolkit, and run the setup again. It always builds again, compiling only what
changed, and when the lifter or its patches have changed it lifts again
first, since a fix to the lifter only reaches the game through a new lift.

One install has both games, and both are built:

| Game | Exe | Builds into | Play with |
|---|---|---|---|
| Red Alert 2 | `game.exe` | `build-game\ra2.exe` | `Red Alert 2 (recomp).cmd` |
| Yuri's Revenge | `gamemd.exe` | `build\ra2.exe` | `Yuri's Revenge (recomp).cmd` |

## Step by step (Windows)

`Setup.cmd` runs exactly these commands. Prerequisites: Windows 10/11,
**Python 3.10+** (`py -3 --version`), **git**, **Visual Studio 2022 or 2026**
(any edition, or the Build Tools) with *Desktop development with C++*
including the x86 tools, **CMake 3.20+** and **Ninja**, and the pcrecomp
toolkit cloned **beside** this repository:

```
some-folder\
  pcrecomp\     <- git clone https://github.com/sp00nznet/pcrecomp
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
   or, from the EA App:
   ```
   robocopy "C:\Program Files\EA Games\Command and Conquer Red Alert II" game /E
   ```
3. Headers, imports and C++ classes (seconds):
   ```
   py -3 ..\pcrecomp\tools\pe\pe_analyze.py game\gamemd.exe --json work\pe_analysis.json
   py -3 ..\pcrecomp\tools\cpp\rtti.py game\gamemd.exe -o work\rtti.json --seeds work\rtti_seeds.json
   ```
   Expected from `rtti.py`: `classes : 954` and `virtual methods : 6,665`.
4. The function catalog (about 15 minutes):
   ```
   py -3 ..\pcrecomp\tools\disasm\disasm32.py game\gamemd.exe -o work\functions.json --seed-functions work\rtti_seeds.json
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
py -3 ..\pcrecomp\tools\pe\pe_analyze.py game\game.exe --json work\game\pe_analysis.json
py -3 ..\pcrecomp\tools\cpp\rtti.py game\game.exe -o work\game\rtti.json --seeds work\game\rtti_seeds.json
py -3 ..\pcrecomp\tools\disasm\disasm32.py game\game.exe -o work\game\functions.json --seed-functions work\game\rtti_seeds.json
py -3 run_lift.py --all --target game
set BUILD_DIR=build-game
set CMAKE_ARGS=-DRA2_TARGET=game
build.cmd
build-game\ra2.exe --run
```

Expected: `Functions: 23191` and `lifted 23201   not-lifted stubs 0   errors 0`.
The playtest suite and conformance take `RA2_TARGET=game` (docs/testing.md).

`PCRECOMP` (environment, for `run_lift.py`) and `-DPCRECOMP=` (CMake) point at
a toolkit checkout other than `..\pcrecomp`; the lifter and the runtime must
come from the same tree. It builds from pcrecomp `main`, for MSVC and clang-cl
alike.

The usual trip-ups: `python` opening the Microsoft Store (that is Windows' alias;
use `py -3`), and a PATH change that needs a new terminal window.

## Linux, native

`build-linux/ra2` (Yuri's Revenge) and `build-linux-game/ra2` (Red Alert 2)
are 32-bit Linux programs: the same lifted C as the Windows build, on
pcrecomp's `runtime/win32hle`, which answers every Windows call the game
makes itself (files, windows and dialogs, DirectDraw and DirectSound on SDL2,
the Bink movies on ffmpeg, Winsock, the registry, COM and save-game storage).
No Wine and no Windows DLLs; the game folder is only data.

On Debian or Ubuntu (`setup.sh` names the Fedora and Arch packages):

```
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install gcc-multilib cmake ninja-build pkg-config python3-venv fonts-liberation \
                 libsdl2-dev:i386 libsdl2-ttf-dev:i386 libavformat-dev:i386 libavcodec-dev:i386 libswresample-dev:i386 libavutil-dev:i386
./setup.sh
```

`setup.sh` links `game/` to your install (it looks in your Steam libraries),
catalogs, lifts and builds both games, and leaves `Yuri's Revenge (recomp).sh`
and `Red Alert 2 (recomp).sh`. By hand, it is *Step by step* with `python3`
for `py -3` and `./build-linux.sh` for `build.cmd` (`BUILD_DIR=build-linux-game
CMAKE_ARGS=-DRA2_TARGET=game` for Red Alert 2), then `build-linux/ra2 --run`.
It needs a current pcrecomp (`main`); `setup.sh` offers to update an older clone.

The window scales like the Windows presenter: F12 cycles sharp, smooth, CRT,
nearest and integer scaling, F11 (or Alt+Enter) is fullscreen, and `ra2.ini`
beside the program remembers them (`--scale` on the command line). F10 opens
the settings menu over the picture: scaling, the bars, fullscreen, HD
vehicles (off until you turn them on), the game's resolution and the mod. It
has no Controls entry: Modern controls are the Windows presenter's. Its
resolution list has your display's own size, and the game's own Options
screen lists the large modes too ([hires.md](hires.md)).
`--headless`, `--record`, `--mute`, `--hd-voxels`, `--seed` and the scripted
input are the Windows host's; `tools/playtest.py` runs the native build when
it is there. LAN games speak IPXEmu's protocol (IPX over UDP), the one the
Windows build uses through the game folder's `wsock32.dll`.

On Debian 13, headless, the playtest suite passes 30 of 30 for each game.
Not on this host: `--original` (it runs the shipping machine code on Windows),
Modern controls, and a LAN game against a Windows player (untried).

## macOS, native

`build-macos/ra2` (Yuri's Revenge) and `build-macos-game/ra2` (Red Alert 2)
are native Mac programs, arm64 on Apple silicon: the same lifted C and the
same win32hle as [Linux, native](#linux-native), with no Wine, CrossOver or
Rosetta. A Mac runs only 64-bit programs, so the guest's 32-bit memory is one
4 GB block of the host's (pcrecomp's `runtime/win32hle/README.md`, *Guest
memory*), at a fixed address (`RA2_MEM_BASE`, 16 TB up) so the lifted code
adds a constant to every address. The game is your copy: Steam for Windows
in a CrossOver bottle, the Mac Steam library, or a folder anywhere (`game/`
a link to it).

`./setup.sh` checks for Xcode's command line tools and offers to install
Homebrew's `cmake ninja pkgconf sdl2 sdl2_ttf ffmpeg uv`, finds the game,
catalogs and lifts each game, builds it with `build-macos.sh`, and leaves
`Yuri's Revenge (recomp).command` and `Red Alert 2 (recomp).command` here to
double-click. By hand:

```
./build-macos.sh                                                    # Yuri's Revenge
BUILD_DIR=build-macos-game CMAKE_ARGS=-DRA2_TARGET=game ./build-macos.sh   # Red Alert 2
build-macos/ra2 --run
```

To give the build to another Mac as an app, `tools/package_macos.py` makes
`dist/<Game>.app` and a zip of it: the host, its libraries copied in and
relinked, the game's icon, and a launcher that asks for the game folder on
the first run. Homebrew's libraries run only on the macOS they were
built on or newer; for an app that runs on any Apple Silicon Mac, build the
libraries with `tools/deps-macos.sh` first (macOS 11 target, about five
minutes) and build the host against them:

```
tools/deps-macos.sh
BUILD_DIR=build-macos-compat CMAKE_ARGS="-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_BUILD_RPATH=$PWD/deps-macos/lib" \
  PKG_CONFIG_PATH=$PWD/deps-macos/lib/pkgconfig ./build-macos.sh
.venv/bin/python tools/package_macos.py --build-dir build-macos-compat
```

The app is ad-hoc signed, not notarized: the first open on another Mac is
a right-click, Open (or Privacy & Security, Open Anyway), after moving the
app out of the download folder with Finder.

The command line, the window (F10, F11, F12), `ra2.ini` and the mods (F9) are
the Linux host's. On a Mac keyboard the F keys are media keys unless you hold
fn (or turn on "Use F1, F2, etc. keys as standard function keys"), and macOS
keeps F11 for Show Desktop: Option+Return is fullscreen. On an Apple silicon Mac the playtest suite passes 30 of 30 for
each game.

## macOS and Linux, under Wine

The same `ra2.exe`, cross-compiled with clang-cl and played under Wine:
CrossOver on a Mac, `wine` on Linux. No Visual Studio and no copy of the
game: `game/` is a link to your install. On a Mac, install Steam for Windows
in a CrossOver bottle and Red Alert 2 from your library in it; on Linux,
install it from Steam (which runs it with Proton) or have the folder anywhere.
Then `./setup.sh --wine`.

On a Mac it checks for and offers to install Homebrew's `llvm`, `lld`,
`cmake`, `ninja`, `xwin` and `uv`. On Linux it lists what your package manager
should install (clang-cl, lld, llvm, cmake, ninja, wine and Python 3) and
offers to download `xwin`'s release binary. Either way it asks before xwin
downloads the x86 MSVC C runtime and Windows SDK from Microsoft (about 1 GB,
under Microsoft's licence, into `~/.xwin`), then catalogs, lifts and builds
each game as `Setup.cmd` does, and leaves `Yuri's Revenge (recomp)` and
`Red Alert 2 (recomp)` launchers here (`.command` on a Mac, `.sh` on Linux).

By hand, the steps are *Step by step*'s with `python3` (or `.venv/bin/python`)
for `py -3` and `./build.sh` for `build.cmd`; `./play.sh` (Yuri's Revenge) or
`./play.sh ra2` runs a build under Wine, with any host flags after it (on a
Mac, `CX_BOTTLE` picks the bottle, default `Steam`). `tools/playtest.py` and
`tools/conformance.py` run the host through Wine off Windows (`RA2_WINE` for
another launcher).

It needs a current pcrecomp (`main`), whose native32 supports Wine: DEP turned on
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
Windows as well (a flaky case, not a Wine one).

Not yet under Wine: recordings (`--record` starts `ffmpeg` through Wine's
`cmd`, which cannot run a Mac or Linux program), and `--original` is untested.

## The command line

`build\ra2.exe` is Yuri's Revenge and `build-game\ra2.exe` Red Alert 2; they
take the same flags.

```
build\ra2.exe                                   # dry run: map and bind, print the entry point
build\ra2.exe --run                             # play: our own window; settings F10, scaling F12, fullscreen F11
build\ra2.exe --run --fullscreen --scale crt     # borderless fullscreen, CRT look
build\ra2.exe --run --classic                   # the original exclusive-fullscreen DirectDraw
build\ra2.exe --run --mod NAME                  # a mod from mods\yr\ (mods\README.md); --mod none for none
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

## Reading the lifted C

The lift (`src/recomp/gen`, made on your machine, never committed) names what
the binary itself names (pcrecomp's `tools/lift/name_lift.py`): virtual
methods by their class and vtable slot from RTTI (`UnitClass__virtual_42`,
COM's by name: `OverlayClass__Load`), constructors, destructors and
`operator_delete` by their shape, and functions that print their own name in
a debug message by it; 7,167 of Yuri's Revenge's functions are named. Each
function has a header saying how it was named, its `this` class, the strings
it uses, the Windows calls it makes and how many places call it, and a
constant that is a string's or a vtable's address has a comment. A function
with no such evidence keeps its address name (`sub_` and its address) and
still gets the header. The address is in each header and in its
`RECOMP_ENTER`, so a crash report's address finds the function.
