# The presenter

`build\ra2.exe --run` shows the game in its own Direct3D 11 window
(`src/runtime/present.c`). The game runs exactly as it does headless: its
display is virtual and its windows are invisible, and the presenter shows the
picture and carries input back. `--classic` is the game's own exclusive
fullscreen DirectDraw, as it shipped.

```
build\ra2.exe --run                               # a window at 85% of the screen's height
build\ra2.exe --run --fullscreen                  # borderless fullscreen
build\ra2.exe --run --scale crt                   # sharp | smooth | crt | nearest | integer
build\ra2.exe --run --classic                     # the original display
```

| Key | |
|---|---|
| F11, Alt+Enter | borderless fullscreen on the window's monitor, and back |
| F12 | scaling: sharp-bilinear, smooth, CRT, nearest, integer |
| F10, or right-click beside the picture | the settings menu |

F10, F11 and F12 can be rebound, and mouse buttons 4 and 5 given a key, in
`[keys]` (see [Input](#input)).

The F10 menu includes **Controls**. **Classic** keeps the original games'
controls. **Modern** uses left-click to select, left-click on open ground to
deselect, and right-click for contextual orders, and adds Queue Five
(Shift-click a build icon to queue five). It keeps the original
right-click-drag camera pan, and active placement and order modes keep their
original right-click cancellation. It applies to both games and is saved as
`controls=classic` or `controls=modern` in `ra2.ini`. The implementation notes
and test record are in [modern-controls.md](modern-controls.md).

## Settings

The settings menu has scaling, bars, fullscreen, HD vehicles, controls,
and the game's resolution. The presenter remembers its own choices, and the
window's place and size, in `build\ra2.ini`:

```ini
[present]
scale=sharp
bars=blur
fullscreen=0
hdvoxels=1
controls=classic
cursorlock=auto
window=100,100,1600,900
```

A `--scale` or `--fullscreen` on the command line wins over the file.

- **Bars.** The 800x600 menus in a 16:9 window or screen leave bars at the
  sides. `blur` (the default) fills them with a soft, darkened copy of the
  picture stretched over the whole window, so the menus sit in a frame rather
  than a hole; `black` is plain letterboxing. The shader draws the fill first
  (`mode 5`, four bilinear taps of the picture's ~32-pixel-wide mip, which the
  presenter regenerates each frame) and the picture on top.
- **HD vehicles** (on unless turned off) draws voxel units at twice the
  resolution of the rest of the picture ([voxels.md](voxels.md)); the
  presenter's picture is then the game's at 2x, up to a game resolution of
  2048x1080. *On, 3x* is sharper and slower. *On, 1x smooth* instead renders
  the units at 2x and averages each 2x2 block back down, antialiasing them at
  the game's own size (`hdvox_aa=1` in `ra2.ini`) -- it costs the same as 2x
  but keeps units vanilla-sized.
- **Game resolution** applies to the next battle (skirmish or mission); the
  menus are always 800x600. It is written to the game's `RA2MD.INI`
  `[Video] ScreenWidth/ScreenHeight`, and to the game's options in memory
  (`0x00A8EB60` +0x24/+0x28, the object the settings reader is called on at
  `0x0052C630`), because the game saves its own options on exit and would put
  the old size back. 3840x2160 needs the sidebar fix in [hires.md](hires.md).
- While the menu is open the picture holds still (the menu runs its own
  message loop on the presenter's thread); the game itself keeps running.

## Input

- **Cursor lock.** In a battle the mouse stays on the picture (`ClipCursor`
  to the picture's rectangle), so edge scrolling works at the edge of an
  ultrawide or a second monitor, and a click near the edge never lands on the
  desktop. It lets go while the game shows its menus (the Windows cursor),
  while the settings menu is open, when the window goes inactive (Alt-Tab, a
  click on another window) or is minimised, and on exit, and takes the mouse
  again when the window comes back. `cursorlock=auto` (the default) has it on
  in Modern controls and off in Classic; `on` and `off` choose. F10, Controls,
  *Keep the mouse in the picture in battle* switches it. The log says
  `[input] cursor clip L,T,R,B` and `[input] cursor released (reason)`.
- **Middle-drag pan** (Modern controls, in a battle). The middle button held
  and moved is the game's own right-drag scroll: once the mouse has moved past
  the Windows drag distance the presenter sends a right-down at the press
  point, the moves with the right button held, and a right-up. The
  battlefield hook starts the native pan at once for that right-down, so it is
  never a right-click order, and it comes before the attack-move cancel. A
  plain middle click sends nothing. While a building is being placed, the
  right-down still reaches the game's placement mode, which cancels on a right
  press as it always has.
- **Rebindable keys.** `[keys]` in `ra2.ini`:

  ```ini
  [keys]
  attackmove=A      ; Modern controls (modern-controls.md)
  settings=F10
  fullscreen=F11    ; Alt+Enter stays
  scaling=F12
  mouse4=none       ; the key mouse button 4 (back) sends
  mouse5=none       ; ... button 5 (forward)
  ```

  A value is a letter or digit, a key name (`F1`-`F24`, `Esc`, `Space`, `Tab`,
  `Enter`, `Home`, `End`, `PgUp`, `PgDn`, the arrows, `Numpad0`-`Numpad9`,
  `Pause`, `Tilde`, ...), or a virtual-key number (`0x41`); `none` unbinds.
  A mouse button's key goes through the same path as the keyboard's, so
  `mouse4=A` arms attack-move and `mouse5=F10` opens the settings. A name the
  table does not have, and a value it cannot read, are logged
  (`[keys] unknown key ...`) and ignored. `--bind name=key` sets one for a run
  and wins over the file.

  The table is `k_keys` in `src/runtime/input.c` with its IDs (`KEY_*`) in
  `input.h`. A feature that needs a key adds an ID and a row (name, label,
  default) and asks `input_key_is(KEY_..., vk)` where it handles keys; the
  loader, `--bind`, the logging and the F10 menu labels come with it, and
  `input_key_ini_name`, `input_key_label`, `input_key_set` and
  `input_key_to_name` are there for a keybinding page.
- **Scroll speed** (`edgescroll=`, `keyscroll=`): not done here. Edge and key
  scrolling move the view by a step per game frame, from the game's own
  `ScrollRate` option (an index into its speed table: `OptionsClass` +0x10,
  `0x00A8EB60` in Yuri's Revenge, read at `0x005FA71D` and saved at
  `0x005FADA1`), so the speed follows the game speed and the presenter can only
  change the index, a whole step at a time, which the game then saves to
  `RA2MD.INI` as the player's own setting. A multiplier that does not depend on
  the game speed needs a patch where the step is applied (`run_lift.py`); that
  is left for the orders track.
- **Raw input** (`WM_INPUT` with a `mousesens=`): dropped after a design look,
  not built. A sensitivity other than Windows' needs a cursor of the
  presenter's own, moved by raw deltas, that no longer matches the real one. In
  a battle the game draws its cursor from what `GetCursorPos` answers, so that
  would work while the cursor lock holds; but the menus (the game's and the
  F10 one) use the Windows cursor, the hit test and the bars use the real
  position, and every switch between them would have to warp the real cursor
  (`SetCursorPos`) to the presenter's, which is the fight with the game's own
  cursor the plan said to stop at. Windows' pointer speed already applies to
  the presenter, and in a battle the game's cursor follows it 1:1.

### Self-test

`--selftest-input S` (a script step, [testing.md](testing.md)) runs in a
battle, `S` seconds in, with no window: the picture is taken as 2x in a wider
client area (`160,0,1440,960` for 640x480) and the messages a player sends go
through the window procedure. The cursor lock logs what it would clip and
never touches the machine's real mouse; what the presenter forwards reaches the
game as in play. `tools/playtest.py`'s `input-selftest` checks that the lock
clips four times and lets go on focus loss, minimise and the settings menu; a
middle-drag reaches the game as the right-drag (`[modern] middle-drag pan`
from the battlefield hook, on the game's thread); a middle click sends nothing;
mouse 4 (`--bind mouse4=A`) arms attack-move and mouse 5 (`--bind mouse5=H`)
posts H to the game; and an unknown `[keys]` name is logged.

## How it fits together

| Part | Where | What it does |
|---|---|---|
| Virtual display | `host.c` (headless DirectDraw) | The game's mode is remembered, not set; its primary is a system-memory surface the host owns |
| Invisible game windows | `host.c` | Layered at alpha 0: painted by Windows, shown nowhere |
| Virtual screen | `host.c` (`vorigin`) | Every screen coordinate the game sees is relative to its main window's client corner |
| Presenter window | `present.c` | Copies the primary each vsync (`host_frame`), uploads it, draws it letterboxed through a shader |
| Input | `present.c`, `input.c` | Maps the player's mouse into game coordinates, posts it to the game window under it, answers `GetCursorPos`/`GetKeyState` |

## Why it is built this way

- **The game is not touched.** Like SimCity 2000's frontend and Gunman's
  presenter, everything sits around the original code. The playtest suite and
  the `--original` oracle test the same display path a player uses.
- **Sharp-bilinear is the default** (Gunman's shader): whole texels stay crisp
  and only their edges blend, so an 800x600 game upscaled 2.9x to a 4K-class
  window neither blurs nor shimmers. Because of that, the window does not need
  to be a whole multiple: it opens at 85% of the work area's height.
- **The presenter's thread is per-monitor DPI aware; the game's is not.**
  Unaware, Windows stretched the window by the display scale (2x at 200%)
  after the shader, and the picture came out blurred. The game's windows stay
  unaware, so nothing about how the game sees its screen changes. The hit test
  that finds the game window under a click runs in their (unaware) context:
  from the aware thread their rectangles came back in physical pixels and no
  button contained the point.
- **The screen is virtual.** The game was written for a fullscreen window whose
  client area is the screen, and offsets its drawing by window positions. Its
  windows now sit wherever Windows (or offstage, which moves every window of a
  program to its monitor) puts them, so `ClientToScreen`, `ScreenToClient`,
  `GetWindowRect`, `WindowFromPoint`, `MoveWindow`, `SetWindowPos` and
  `SetCursorPos` are answered relative to the main window's client corner.
  Without it, the menu's button captions were drawn off the surface.
- **A click goes to the deepest visible game window under it.** The main
  menu's dialog is not a direct child of the main window, so asking the main
  window's children found nothing. Static controls pass clicks to what is
  under them, as Windows does.
- **The cursor is the game's, either way.** In a battle the game captures the
  mouse and draws its cursor into the picture, so the real one is hidden over
  the window. The menus are Win32 dialogs and use the Windows cursor the game
  sets (its own arrow, resource 0x68), so while the mouse is not captured the
  presenter shows that one; before, the menus had no cursor at all. It
  switches when a battle starts or ends, without waiting for the mouse to move.
- **Under Wine the game's windows are kept out of the way by hand.** On a Mac
  (CrossOver) a button the presenter clicked called `SetFocus`, Wine
  activated its invisible top-level window and the Mac made it the key
  window: the presenter went inactive, the Mac's own cursor came back, and
  the button's capture took the real mouse, so the button-up arrived at its
  real place on the screen and no click ever finished. Under Wine only, a CBT
  hook on the game's thread refuses that activation (keys go where the game
  asked for the focus), `SetForegroundWindow`, `SetActiveWindow` and
  `BringWindowToTop` are answered without the call, and a message hook drops
  any real mouse message that reaches a game window and hands it to the
  presenter, which forwards it like the rest. The host notes every mouse
  message it posts (`input_post_mouse`), which is how the hook tells them apart.
- **Mouse buttons are what the presenter saw.** The game's controls read
  `VK_LBUTTON` as well as the messages, and a click has to agree whether it
  came from a mouse, a pen or a test driver.

## Testing it

`tools\present_drive.ps1` clicks the presenter window the way a player does,
at game coordinates converted through the same letterboxing:

```
powershell -File tools\present_drive.ps1 -Clicks "20:720,220 26:720,300 33:714,262"
```

That is Single Player, Skirmish and Start Game, and the log shows each click
reaching its button:

```
[present] click at game 720,220 -> Button 05E5215C (id 1667) at 76,21
[present] click at game 720,300 -> Button 033C2B06 (id 1401) at 76,17
[present] click at game 714,262 -> Button 01F52A42 (id 1559) at 70,21
[game] Capture_Mouse()
```
