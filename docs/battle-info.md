# Battle info

The F10 menu's **Battle info** entry adds three things to a battle, in Red
Alert 2 and Yuri's Revenge. Each is off by default, which is the game as it
shipped, and the presenter keeps the choice in `ra2.ini`:

| Setting (`[present]`) | Values | What it does |
| --- | --- | --- |
| `healthbars=` | `selected` (default), `damaged`, `always` | Which objects show a health bar. `selected` is the original: selected objects, and the one under the mouse. `damaged` adds every object below full strength; `always`, every object. |
| `buildinfo=` | `0` (default), `1` | The cameo being built shows its percentage where a finished one says "Ready", and a cameo's tooltip adds its build time under the cost. |
| `overlay=` | `0` (default), `1` | A line at the top of the battlefield: game clock, frames a second, orders a minute. |

For a headless run, `--healthbars MODE`, `--buildinfo` and `--overlay` set
them on the host's command line, and win over `ra2.ini`.

The code is `src/runtime/battleinfo.c`, the lift's patches are
`battle_info_patches` in `run_lift.py`, and the overlay is drawn by the
presenter (`draw_overlay` in `src/runtime/present.c`). The native Linux and
macOS host has none of it yet: its stubs leave every setting off.

## Health bars

TechnoClass's extras (Yuri's Revenge `0x006F5190`, Red Alert 2 `0x006C2C40`)
draw the bar of a selected object, then of one the mouse is over and that is
not selected. The patch is on that test: for an object that is neither, the
host may answer "the mouse is over it", and the game draws the bar the way it
draws a hovered one, with its own colours and pips. `damaged` compares the
object's Health with its type's Strength, the pair the game's own
GetHealthPercentage divides.

## Build percentage and time

The sidebar's cameo loop reads each cameo's text ("Ready", or nothing while it
builds) and draws it centred at the top of the cameo, in the game's font and
box. For a cameo whose factory is building and not on hold (the same tests the
"On Hold" text makes) the host gives it "42%" instead of nothing: the
factory's stage out of 54.

The tooltip ("Name" and "$cost") gains a line with the type's build time from
its own GetBuildSpeed (cost, the rules' `BuildSpeed`, the type's
`BuildTimeMultiplier`), in game seconds of 15 frames, as at normal speed. Low
power and extra factories change the real time, which this does not show: it
is the list time, as the cost is the list price.

## Overlay

`12:34  60 FPS  85 APM`, white on a dark box at the top centre of the
battlefield, drawn into the game's picture before it is scaled (so it scales
with it, one font pixel per game pixel per 480 lines).

- **Clock**: the game's frame count over 15, the game's own seconds at normal
  speed. It starts again with each game.
- **FPS**: game frames in the last second (the game's speed, not the
  presenter's pictures).
- **APM**: orders in the last 60 seconds. An order is an event the player puts
  in the game's outgoing list (move, attack, build, place, deploy, sell...):
  every inline add into that list is counted (some 36 sites in each game).
  Selecting is not an order.

The line shows only while the game's frame count moves, so not in the menus or
while the game is paused.

## Tests

`skirmish-battleinfo` (tools/playtest.py) plays the start of
`skirmish-build` with all three on. The host logs each feature's first use
(`[battleinfo] first health bar shown by healthbars=always`, `first build
percentage: 0%`, `tooltip build time 0:34 (503 frames)` for the power plant,
`first order counted`), and every 10 seconds in game the overlay's line with
the counts behind it. From Yuri's Revenge, near the end of the run:

    [battleinfo] overlay 6:55  63 FPS  13 APM; health bars always, 152230 forced; build % drawn 107 times; 15 orders

Headless, the game runs at about 63 frames a second, so its clock runs four
times faster than the wall's. The game's log doesn't name every event it
sends, so the count (15) is above its "Adding event" lines (7); which events
the other eight were is not yet broken down.

The pictures themselves (the bars, the percentage, the overlay in the
presenter) are for a look by hand.
