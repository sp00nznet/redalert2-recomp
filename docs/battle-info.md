# Battle info

The F10 menu's **Battle info** entry adds two things to a battle, in Red
Alert 2 and Yuri's Revenge. Each is off by default, which is the game as it
shipped, and the presenter keeps the choice in `ra2.ini`:

| Setting (`[present]`) | Values | What it does |
| --- | --- | --- |
| `healthbars=` | `selected` (default), `damaged`, `always` | Which objects show a health bar. `selected` is the original: selected objects, and the one under the mouse. `damaged` adds every object below full strength; `always`, every object. |
| `buildinfo=` | `0` (default), `1` | The cameo being built shows its percentage where a finished one says "Ready", and a cameo's tooltip adds its build time under the cost. |

For a headless run, `--healthbars MODE` and `--buildinfo` set them on the host's command line, and win over `ra2.ini`.

The code is `src/runtime/battleinfo.c`, the lift's patches are
`battle_info_patches` in `run_lift.py`. The native Linux and
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

## Tests

`skirmish-battleinfo` (tools/playtest.py) plays the start of
`skirmish-build` with both on. The host logs each feature's first use
(`[battleinfo] first health bar shown by healthbars=always`, `first build
percentage: 0%`, `tooltip build time 0:34 (503 frames)` for the power plant),
and every 10 seconds the counts (`health bars always, 152230 forced; build %
drawn 107 times`).

The pictures themselves (the bars on every unit, the percentage, the tooltip)
are for a look by hand; the contact sheet shows the bars and the tooltip.
