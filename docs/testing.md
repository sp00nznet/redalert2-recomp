# Testing: walking every menu and game mode

`tools/playtest.py` drives the recompiled game headless through every menu
screen and game mode, several runs at a time, and keeps the evidence: a log, a
recording and a contact sheet per case. `--original` runs the same script on
the shipping machine code, which is how a failure is pinned on the lift or on
the host.

It is Civilization III's framework (civ3 `tools/playtest.py`,
`src/runtime/input.c`, `src/runtime/oracle.c`) adapted to RA2. It also takes
Burnout 3's state signal: waiting on what the game says it is doing.

```
py -3 tools/playtest.py --list
py -3 tools/playtest.py 'menu-*' 'back-*' --jobs 3
py -3 tools/playtest.py skirmish-loop
py -3 tools/playtest.py skirmish-loop --original
```

## Cases

| Family | What it does | Passes when |
|---|---|---|
| `menu-*` | each main-menu button | the screen it opens is open (`[dialog] open 0x...`) |
| `back-*` | open a screen, its Back/Main Menu, then Exit Game | the main menu came back and the game exited 0 |
| `sp-*` | Single Player's New Campaign, Load Saved Game, Skirmish | the screen opened |
| `options-*`, `movies-*` | sub-screens and the movie/credit players | the screen opened, no fault |
| `skirmish-start` | Single Player, Skirmish, Start Game | in game: `Capture_Mouse()` logged and the picture moving |
| `skirmish-build` | deploy the MCV, build a power plant and a barracks (placed at the first free spot of a ring around the yard), train a GI, all by mouse | the game's event log: `PRODUCE` three times, `PLACE`; the player alive (the Soviet AI's first rush arrives at about 3:40, so the orders have to be in by then) |
| `skirmish-loop` | a skirmish nobody plays, through defeat, the score screen, Continue | the main menu opens again; the AI usually wins in about 4.5 minutes, but now and then not within the 12, and the case fails on timing (run it again) |
| `campaign-*` | New Campaign, then the Allied or Soviet emblem | in game, and the player not defeated |
| `lan-new` | Network, then New: the LAN host's setup screen | the screen opened, no fault |

## Red Alert 2 (`game.exe`)

The same cases run on Red Alert 2 itself with `RA2_TARGET=game` (its build
in `build-game\`, its dialogs mapped from `game.exe`, results in
`work\tests-game\`):

```
set RA2_TARGET=game
py -3 tools/playtest.py --jobs 3
```

Its skirmishes start at the fastest speed, uncapped, so the script moves
the speed slider one notch down before Start Game (bringup.md, section 13).

## How a script is written

Buttons are named, not clicked by pixel. RA2's menus are Win32 dialog
resources (98 of them in `gamemd.exe`), and each control's caption is a string
key such as `GUI:Skirmish`. `tools/dialogs.py` maps every dialog to its
controls (`py -3 tools/dialogs.py --show 0x100`), and a case says:

```python
S().press(MAIN, 'SinglePlayer', SINGLE).press(SINGLE, 'Skirmish', SKIRMISH)
   .press(SKIRMISH, 'StartGame').waitlog('Capture_Mouse').move(236, 240, after=2)
   .key('0x48', after=10)
```

which becomes host arguments:

```
--press 0xE2:1667@1 --press 0x100:1401@2 --press 0x102:1559@3 --waitlog Capture_Mouse@4
--move 236,240@7 --key 0x48@18
```

| Event | Meaning |
|---|---|
| `--press DLG:CTRL@s` | wait until dialog DLG is open, then click control CTRL |
| `--select DLG:CTRL=N@s` | pick item N of a list or combo box in DLG, or set a slider to N |
| `--waitlog TEXT@s` | hold the script until the game's debug log prints TEXT |
| `--key [c][s][a]+vk@s`, `--move x,y@s`, `--click [c][s][a]+x,y@s`, `--wait VA@s` | as in civ3; a click can hold Ctrl, Shift, Alt (Ctrl+click is force-fire) |
| `--drag x1,y1,x2,y2@s` | a band selection: button down at one corner, across, up at the other |
| `--uclick x,y@s` | the native host only: a click as the mouse gives it, to the window under the point (a menu's own control, a combo box's arrow), where `--click` goes to the game's window |

`s` counts seconds from when the main menu first opened (civ3: then a script
does not depend on how long the boot took). A press and a waitlog wait for
their state, and every later event moves back by the wait, so times only need
to be in order.

## Why it works this way

- **Dialogs are identified by resource ID.** The game calls
  `FindResourceA(…, id, RT_DIALOG)` right before `CreateDialogIndirectParamA`,
  so the host tags each dialog window with its ID as it is created, and prints
  `[dialog] open 0x100` / `[dialog] closed 0x100`. Those lines are the menu
  milestones.
- **A press is a real click first.** The cursor moves to the control's centre,
  `WM_LBUTTONDOWN`/`UP` are posted to it, and the button is held until the game
  has read `VK_LBUTTON` (civ3: a fixed hold was missed under load). Only if
  the dialog neither closes nor opens another within 3 s does the host send
  the `BN_CLICKED` a button sends its parent. Every press so far went through
  as a click.
- **Nothing touches the real cursor or keyboard.** Input is posted window
  messages, and the shimmed `GetCursorPos`, `GetKeyState` and
  `GetAsyncKeyState` answer from the script, so a run cannot be disturbed by
  the console (or the RDP phone) and cannot disturb it.
- **In game is a log line.** `--waitlog` reads the game's own debug log (the
  printf the retail build compiled out, given back by `run_lift.py` `HOOKS`).
  `Capture_Mouse()` is printed when a game starts. It replaced a fixed
  40-second wait that two runs in parallel overran.
- **Each case gets its own game folder**, hard links to `game/` with
  `RA2MD.INI` copied and the intro switched off, so parallel runs cannot
  clobber each other's settings and saves. The game's single-instance mutexes
  get a per-process name under `--headless` for the same reason.
- **`--original` is the oracle.** `src/runtime/oracle.c` maps `gamemd.exe` with
  its code executable and runs it natively in the same host, under the same
  shims (reached through a thunk that adapts a native call to the shims'
  lifted calling convention) and the same script. A case that fails on the lift
  and passes there is a lift bug; one that fails on both is the host's.

## What the oracle has settled

- **The idle player losing is the game, not a bug.** With nobody at the
  controls and the human starting at cell 52,97, the AI wins in about 4.5
  minutes on the shipping code too. That is why `skirmish-loop` expects the
  score screen rather than survival.
- **The black battlefield was the script's cursor.** Left on a menu button at
  x=720, it was past the right edge of the 640x480 game, and edge scrolling
  held the view on the map's black margin, on both machines. The host keeps
  the cursor on screen across a mode change, and in-game cases move it to the
  middle of the battlefield.
- **A crash at the mode switch and at exit was the host's**: the recorder
  locked a primary surface the game had released. The host now holds its own
  reference and drops it when DirectDraw goes.
- **Exit Game hanging was the lift's**: a spin-wait on the sound thread under
  native32's machine lock (pcrecomp #42).
- **The LAN setup screen crashing was the lift's**: `push; jmp` to the
  procedure's own epilogue read as a call (pcrecomp #43).

The bring-up log has the details ([bringup.md](bringup.md), 11).

## Reading the results

One line per case:

```
pass sp-skirmish          exit 4       dialogs E2 100 102
FAIL skirmish-start       exit 4       dialogs E2 100 102 108; in game, 11 distinct frames; human defeated  !! ...
```

Exit 4 is the watchdog, the normal end of a timed run. The evidence is in
`work/tests/<case>/`: `run.log`, `run.mp4` and `sheet.png`. Reading the sheets
is the real test: a pass says the case reached what it expected and ran clean,
not that every pixel was right.

`RA2_EXE` runs the suite on another build of the host, a clang-cl build for
instance; every run is muted (`--mute`).

"Ran clean" means no fault, no `[not-lifted]`, no unresolved `ITAIL` or
`ICALL`, no message box. An unresolved indirect call returns 0 and the game
carries on, so it fails the case: the voxel rasterizers were one, and the
suite passed with every vehicle invisible until they were counted
([bringup.md](bringup.md) 12).

## RA2 against RA2 over the LAN

Two PCs on one subnet, each with the game, each playing a script in the
game's own input, in the presenter window:

```
build\ra2.exe --run --mute --args tools\lan\host.args      # the PC that hosts
build\ra2.exe --run --mute --args tools\lan\joiner.args    # the PC that joins
```

The host goes Network, New (the host's game screen, `0xBC`); the joiner goes
Network, picks the host's game in the list, Join and Accept (`0xBD`); then
the host's Start Game, and both reach the game (`Capture_Mouse` in the log).

- **Pictures come from the game**, not the screen: the scripts write the
  game's frame every 10 s (`--hd-voxels-dump lan-frames`). A screen capture of
  the window picks up whatever is over it, which on a machine someone uses can
  be their own programs.
- **One subnet.** IPXEmu (the game folder's `wsock32.dll`) finds games by UDP
  broadcast.
- **Two player names**: each PC's `RA2MD.INI` `[MultiPlayer] Handle` (hex)
  must differ.
- The host's game is item 1 of the lobby's games list; item 0 is the lobby.
- After the start the scripts move the cursor onto the battlefield: left on
  a menu button it sits past the 640x480 edge, and edge-scrolling runs the
  view into the map's black margin.
